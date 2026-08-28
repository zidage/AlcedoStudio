//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <json.hpp>

#include "edit/operators/GPU_kernels/opencl_param.hpp"

namespace alcedo {

/** @brief Resolve the CPU ODT model into the OpenCL DRT parameter layout. */
[[nodiscard]] auto ResolveOpenClDrtParams(const nlohmann::json& odt_json)
    -> OpenCL::Pipeline::OpenClToOutputParams;

}  // namespace alcedo

#endif  // HAVE_OPENCL
