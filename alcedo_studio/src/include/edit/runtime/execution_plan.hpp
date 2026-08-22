//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
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

struct CompiledAdjustment {
  AdjustmentInstanceId instance_id;
  OperatorTypeId       type;
};

/**
 * @brief Compiled backend work for one graph. Does not own GPU memory.
 *
 * Always includes GeometryResample so a viewport change does not recompile.
 * The encoder skips the kernel when @ref encode_geometry_resample is false.
 */
struct ExecutionPlan {
  std::vector<GpuPassDesc>        passes;
  GraphValueId                    develop_output{NodeId{"develop"}, PortId{"image"}};
  DevelopCompileSource            source{};
  ResolvedRenderGeometry          geometry{};
  bool                            encode_geometry_resample = false;
  std::size_t                     peak_transient_bytes     = 0;
  std::vector<CompiledAdjustment> primary_grade_adjustments;

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
