//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/pass_encoder.hpp"

namespace alcedo {

// O0 does not specialize PassEncoder. PlanExecutor uses the primary template, which
// throws with the pass kind name and does not select CPU, CUDA, Metal, or the old
// OpenCL pipeline.

}  // namespace alcedo

#endif  // HAVE_OPENCL
