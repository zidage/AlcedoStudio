//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/operators/GPU_kernels/opencl_param.hpp"

namespace alcedo::OpenCL::Pipeline {

static_assert(sizeof(OpenClFusedParams) > sizeof(OpenClToOutputParams));
static_assert(alignof(OpenClFusedParams) == alignof(float));

}  // namespace alcedo::OpenCL::Pipeline

#endif
