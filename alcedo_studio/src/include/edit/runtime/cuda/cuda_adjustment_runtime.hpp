//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo {

using CudaAdjustmentBehavior = AdjustmentBehavior;

[[nodiscard]] inline auto TryResolveCudaAdjustmentBehavior(const OperatorTypeId& type)
    -> std::optional<CudaAdjustmentBehavior> {
  return TryResolveAdjustmentBehavior(type);
}

[[nodiscard]] inline auto ResolveCudaAdjustmentBehavior(const OperatorTypeId& type)
    -> CudaAdjustmentBehavior {
  return ResolveAdjustmentBehavior(type);
}

[[nodiscard]] inline auto IsCudaLocalToneBehavior(CudaAdjustmentBehavior behavior) -> bool {
  return IsLocalToneBehavior(behavior);
}

}  // namespace alcedo
