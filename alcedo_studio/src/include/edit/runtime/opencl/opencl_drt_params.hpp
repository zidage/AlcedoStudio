//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/operators/utils/color_utils.hpp"
#include "edit/runtime/opencl/opencl_drt_gpu_params.hpp"

namespace alcedo {

/**
 * @brief Copy a resolved DRT output transform into the OpenCL DRT parameter layout.
 *
 * ACES 2.0 copies the four lookup tables inline; OpenDRT leaves them zero. Pure value function.
 *
 * @throws std::runtime_error when an ACES 2.0 transform has no resolved tables.
 */
[[nodiscard]] auto PackOpenClDrtParams(const ColorUtils::TO_OUTPUT_Params& resolved)
    -> OpenCL::Pipeline::OpenClToOutputParams;

}  // namespace alcedo

#endif  // HAVE_OPENCL
