//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "edit/runtime/develop_compile_source.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "io/image/export_color_profile_config.hpp"

namespace alcedo {

/**
 * @brief Unique compiled-pass identity. Distinguishes repeated kinds within and across nodes.
 *
 * @ref ordinal is the 0-based index among passes with the same @ref kind in the plan.
 */
struct PassInstanceId {
  NodeId        owner;
  GpuPassKind   kind    = GpuPassKind::UploadRaw;
  std::uint32_t ordinal = 0;

  friend auto operator==(const PassInstanceId& lhs, const PassInstanceId& rhs) -> bool {
    return lhs.owner == rhs.owner && lhs.kind == rhs.kind && lhs.ordinal == rhs.ordinal;
  }
  friend auto operator!=(const PassInstanceId& lhs, const PassInstanceId& rhs) -> bool {
    return !(lhs == rhs);
  }
  friend auto operator<(const PassInstanceId& lhs, const PassInstanceId& rhs) -> bool {
    if (lhs.owner != rhs.owner) {
      return lhs.owner < rhs.owner;
    }
    if (lhs.kind != rhs.kind) {
      return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
    }
    return lhs.ordinal < rhs.ordinal;
  }
};

/** @brief Logical type of a compiled value. Used to reject invalid pass input bindings. */
enum class CompiledValueKind : std::uint8_t {
  SceneImage   = 0,
  Mask         = 1,
  DisplayImage = 2,
};

/** @brief One consumer input port bound to a producer value. */
struct CompiledPassInput {
  PortId            port;
  GraphValueId      source;
  CompiledValueKind expected_kind = CompiledValueKind::SceneImage;
};

/** @brief One produced graph value. */
struct CompiledPassOutput {
  GraphValueId      value;
  CompiledValueKind kind = CompiledValueKind::SceneImage;
};

struct GpuPassDesc {
  GpuPassKind                         kind = GpuPassKind::UploadRaw;
  PassInstanceId                      instance;
  NodeId                              owner;
  std::optional<AdjustmentInstanceId> adjustment;
  std::vector<AdjustmentInstanceId>   parameters;
  std::vector<CompiledPassInput>      inputs;
  std::vector<CompiledPassOutput>     outputs;
};

/**
 * @brief Build a compiled pass with identity, owner, and explicit bindings.
 *
 * @param kind GPU operation.
 * @param owner Document node that owns the work.
 * @param ordinal 0-based index among passes of @p kind already in the plan.
 * @param inputs Consumer ports and producer values.
 * @param outputs Produced values.
 * @param adjustment Adjustment instance when the pass is one adjustment; empty otherwise.
 * @param parameters Stable parameter slot ids read by this pass; no GPU addresses.
 */
[[nodiscard]] inline auto MakeGpuPass(GpuPassKind kind, NodeId owner, std::uint32_t ordinal,
                                      std::vector<CompiledPassInput>            inputs,
                                      std::vector<CompiledPassOutput>           outputs,
                                      std::optional<AdjustmentInstanceId>       adjustment = {},
                                      std::vector<AdjustmentInstanceId>         parameters = {})
    -> GpuPassDesc {
  GpuPassDesc pass;
  pass.kind        = kind;
  pass.owner       = std::move(owner);
  pass.instance    = PassInstanceId{pass.owner, kind, ordinal};
  pass.adjustment  = std::move(adjustment);
  pass.parameters  = std::move(parameters);
  pass.inputs      = std::move(inputs);
  pass.outputs     = std::move(outputs);
  return pass;
}

/** @brief Backend algorithm selected at graph compile time for one grade adjustment. */
enum class CompiledAdjustmentAlgorithm : std::uint8_t {
  Pointwise,
  LocalLaplacian,
  Neighborhood,
};

struct CompiledAdjustment {
  AdjustmentInstanceId        instance_id;
  OperatorTypeId              type;
  CompiledAdjustmentAlgorithm algorithm = CompiledAdjustmentAlgorithm::Pointwise;
};

/** @brief Fused or standalone Grade work produced from the Model list. */
enum class CompiledGradeStageKind : std::uint8_t {
  Pointwise,
  LocalLaplacian,
  Neighborhood,
};

struct CompiledGradeStage {
  CompiledGradeStageKind kind  = CompiledGradeStageKind::Pointwise;
  std::uint32_t          begin = 0;
  std::uint32_t          count = 0;
};

enum class CompiledMaskKind : std::uint8_t { Analytic, Raster };

struct CompiledMask {
  NodeId           node_id;
  CompiledMaskKind kind = CompiledMaskKind::Analytic;
};

/**
 * @brief Compiled Color Grade node: adjustments, mix/mask bindings, and scene I/O.
 *
 * @ref scene_input is the connected producer value (edge order), not a container index.
 */
struct CompiledGradeNode {
  NodeId                          node_id;
  GraphValueId                    scene_input{NodeId{""}, PortId{"image"}};
  GraphValueId                    scene_output{NodeId{""}, PortId{"image"}};
  std::vector<CompiledAdjustment> adjustments;
  std::vector<CompiledGradeStage> stages;
  std::optional<CompiledMask>     mask;
  GraphValueId                    mask_output{NodeId{""}, PortId{"mask"}};
};

/** @brief One DRT/Post neighborhood adjustment or the display transform. */
enum class CompiledDrtStepKind : std::uint8_t {
  Neighborhood     = 0,
  DisplayTransform = 1,
};

struct CompiledDrtStep {
  CompiledDrtStepKind             kind = CompiledDrtStepKind::DisplayTransform;
  std::optional<AdjustmentInstanceId> instance_id;
  OperatorTypeId                  type;
  GraphValueId                    input{NodeId{""}, PortId{"image"}};
  GraphValueId                    output{NodeId{""}, PortId{"image"}};
};

/**
 * @brief Compiled DRT/Post node: neighborhood steps then display transform.
 *
 * @ref scene_input is Develop output when the backbone has no Color Grade.
 */
struct CompiledDrtNode {
  NodeId                          node_id{NodeId{"drt"}};
  GraphValueId                    scene_input{NodeId{"develop"}, PortId{"image"}};
  GraphValueId                    scene_output{NodeId{"drt"}, PortId{"runtime.scene_post"}};
  GraphValueId                    display_output{NodeId{"drt"}, PortId{"display"}};
  std::vector<CompiledAdjustment> post_adjustments;
  std::vector<CompiledDrtStep>    steps;
};

/**
 * @brief Identity of a static compiled plan. Parameter values and viewport are omitted.
 *
 * Source layout is the decoded host/develop extents, not ResolvedRenderGeometry.
 */
struct StaticPlanKey {
  std::uint64_t        topology_hash = 0;
  DevelopCompileSource source_layout{};
  std::uint32_t        backend_capability_version = 0;
};

inline auto operator==(const StaticPlanKey& a, const StaticPlanKey& b) -> bool {
  return a.topology_hash == b.topology_hash && a.source_layout == b.source_layout &&
         a.backend_capability_version == b.backend_capability_version;
}

inline auto operator!=(const StaticPlanKey& a, const StaticPlanKey& b) -> bool { return !(a == b); }

inline auto operator<(const StaticPlanKey& a, const StaticPlanKey& b) -> bool {
  if (a.topology_hash != b.topology_hash) {
    return a.topology_hash < b.topology_hash;
  }
  if (a.backend_capability_version != b.backend_capability_version) {
    return a.backend_capability_version < b.backend_capability_version;
  }
  if (a.source_layout.kind != b.source_layout.kind) {
    return a.source_layout.kind < b.source_layout.kind;
  }
  if (a.source_layout.host_extent.width != b.source_layout.host_extent.width) {
    return a.source_layout.host_extent.width < b.source_layout.host_extent.width;
  }
  if (a.source_layout.host_extent.height != b.source_layout.host_extent.height) {
    return a.source_layout.host_extent.height < b.source_layout.host_extent.height;
  }
  if (a.source_layout.develop_output_extent.width != b.source_layout.develop_output_extent.width) {
    return a.source_layout.develop_output_extent.width <
           b.source_layout.develop_output_extent.width;
  }
  if (a.source_layout.develop_output_extent.height !=
      b.source_layout.develop_output_extent.height) {
    return a.source_layout.develop_output_extent.height <
           b.source_layout.develop_output_extent.height;
  }
  if (a.source_layout.full_reference_extent.width != b.source_layout.full_reference_extent.width) {
    return a.source_layout.full_reference_extent.width <
           b.source_layout.full_reference_extent.width;
  }
  if (a.source_layout.full_reference_extent.height !=
      b.source_layout.full_reference_extent.height) {
    return a.source_layout.full_reference_extent.height <
           b.source_layout.full_reference_extent.height;
  }
  if (a.source_layout.downsample_passes != b.source_layout.downsample_passes) {
    return a.source_layout.downsample_passes < b.source_layout.downsample_passes;
  }
  if (a.source_layout.sensor_active_area.x != b.source_layout.sensor_active_area.x) {
    return a.source_layout.sensor_active_area.x < b.source_layout.sensor_active_area.x;
  }
  if (a.source_layout.sensor_active_area.y != b.source_layout.sensor_active_area.y) {
    return a.source_layout.sensor_active_area.y < b.source_layout.sensor_active_area.y;
  }
  if (a.source_layout.sensor_active_area.width != b.source_layout.sensor_active_area.width) {
    return a.source_layout.sensor_active_area.width < b.source_layout.sensor_active_area.width;
  }
  return a.source_layout.sensor_active_area.height < b.source_layout.sensor_active_area.height;
}

/**
 * @brief Compiled backend work for one graph. Does not own GPU memory.
 *
 * Always includes GeometryResample so a viewport change does not recompile.
 * @ref geometry and @ref encode_geometry_resample are per-frame and are filled by
 * GraphCompiler::BindFrameGeometry. When @ref encode_geometry_resample is false the
 * encoder aliases `geometry.scene_source` onto `develop.sensor_linear` instead of
 * copying a second full-resolution texture.
 *
 * Color Grades are @ref grade_nodes in backbone edge order. DRT/Post is @ref drt.
 * @ref peak_transient_bytes is a compiler exclusive-stage upper bound for tests.
 * SensorDevelop allocation uses a conservative initial slab plus observed high-water,
 * not this field as a Reserve argument.
 */
struct ExecutionPlan {
  StaticPlanKey                           static_key{};
  std::vector<GpuPassDesc>                passes;
  GraphValueId                            sensor_linear_output{NodeId{"develop"},
                                                               PortId{"sensor_linear"}};
  GraphValueId                            geometry_output{NodeId{"geometry"}, PortId{"scene_source"}};
  GraphValueId                            develop_output{NodeId{"develop"}, PortId{"image"}};
  DevelopCompileSource                    source{};
  ResolvedRenderGeometry                  geometry{};
  bool                                    encode_geometry_resample = false;
  std::size_t                             peak_transient_bytes     = 0;
  std::vector<CompiledGradeNode>          grade_nodes;
  CompiledDrtNode                         drt;
  GraphValueId                            display_output{NodeId{"drt"}, PortId{"display"}};
  std::optional<ExportColorProfileConfig> output_color_override;

  [[nodiscard]] auto Contains(GpuPassKind kind) const -> bool {
    for (const auto& pass : passes) {
      if (pass.kind == kind) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief First pass of @p kind, or -1.
   *
   * Prefer @ref FindPass or @ref PassesOfKind when the kind can repeat.
   */
  [[nodiscard]] auto IndexOf(GpuPassKind kind) const -> int {
    for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
      if (passes[static_cast<std::size_t>(i)].kind == kind) {
        return i;
      }
    }
    return -1;
  }

  /** @brief Every pass of @p kind, in plan order. Empty when the kind is absent. */
  [[nodiscard]] auto PassesOfKind(GpuPassKind kind) const -> std::vector<const GpuPassDesc*> {
    std::vector<const GpuPassDesc*> found;
    for (const auto& pass : passes) {
      if (pass.kind == kind) {
        found.push_back(&pass);
      }
    }
    return found;
  }

  /** @brief Pass with @p id, or null. */
  [[nodiscard]] auto FindPass(const PassInstanceId& id) const -> const GpuPassDesc* {
    for (const auto& pass : passes) {
      if (pass.instance == id) {
        return &pass;
      }
    }
    return nullptr;
  }

  /** @brief Compiled Color Grade with @p id, or null. */
  [[nodiscard]] auto FindGrade(const NodeId& id) const -> const CompiledGradeNode* {
    for (const auto& grade : grade_nodes) {
      if (grade.node_id == id) {
        return &grade;
      }
    }
    return nullptr;
  }

  /**
   * @brief Scene value consumed by DRT/Post: last Color Grade output, or Develop when none.
   */
  [[nodiscard]] auto SceneInputForDrt() const -> GraphValueId {
    if (grade_nodes.empty()) {
      return develop_output;
    }
    return grade_nodes.back().scene_output;
  }

  /** @brief First compiled Color Grade, or null when the backbone has none. */
  [[nodiscard]] auto FirstGrade() const -> const CompiledGradeNode* {
    return grade_nodes.empty() ? nullptr : &grade_nodes.front();
  }

  /** @brief Last compiled Color Grade, or null when the backbone has none. */
  [[nodiscard]] auto LastGrade() const -> const CompiledGradeNode* {
    return grade_nodes.empty() ? nullptr : &grade_nodes.back();
  }
};

/**
 * @brief Reject missing producers, duplicate outputs, type mismatches, and out-of-order consumers.
 *
 * @param plan Compiled plan. Does not allocate GPU memory or read parameter values.
 * @throws std::runtime_error when a binding is invalid.
 */
void ValidateExecutionPlan(const ExecutionPlan& plan);

}  // namespace alcedo
