//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "edit/runtime/develop_compile_source.hpp"
#include "edit/runtime/pass_kind.hpp"

namespace alcedo {

struct GpuPassDesc {
  GpuPassKind kind = GpuPassKind::UploadRaw;
};

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
 * @ref peak_transient_bytes is a compiler exclusive-stage upper bound for tests.
 * SensorDevelop allocation uses a conservative initial slab plus observed high-water,
 * not this field as a Reserve argument.
 */
struct ExecutionPlan {
  StaticPlanKey                   static_key{};
  std::vector<GpuPassDesc>        passes;
  GraphValueId                    sensor_linear_output{NodeId{"develop"}, PortId{"sensor_linear"}};
  GraphValueId                    geometry_output{NodeId{"geometry"}, PortId{"scene_source"}};
  GraphValueId                    develop_output{NodeId{"develop"}, PortId{"image"}};
  DevelopCompileSource            source{};
  ResolvedRenderGeometry          geometry{};
  bool                            encode_geometry_resample = false;
  std::size_t                     peak_transient_bytes     = 0;
  std::vector<CompiledAdjustment> primary_grade_adjustments;
  std::vector<CompiledGradeStage> primary_grade_stages;
  std::optional<CompiledMask>     primary_grade_mask;
  GraphValueId                    mask_output{NodeId{""}, PortId{"mask"}};
  GraphValueId                    primary_grade_output{NodeId{"grade.primary"}, PortId{"image"}};
  GraphValueId                    display_output{NodeId{"drt"}, PortId{"display"}};

  [[nodiscard]] auto              Contains(GpuPassKind kind) const -> bool {
    for (const auto& pass : passes) {
      if (pass.kind == kind) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] auto IndexOf(GpuPassKind kind) const -> int {
    for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
      if (passes[static_cast<std::size_t>(i)].kind == kind) {
        return i;
      }
    }
    return -1;
  }
};

}  // namespace alcedo
