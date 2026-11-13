//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <stdexcept>
#include <string>

namespace alcedo {

/**
 * @brief Throw if @p status is not CL_SUCCESS.
 * @param status OpenCL status from the preceding call.
 * @param what Short name of the failing API.
 * @throws std::runtime_error with the API name and numeric status.
 */
inline void CheckOpenCl(const cl_int status, const char* what) {
  if (status != CL_SUCCESS) {
    throw std::runtime_error(std::string(what) + ": OpenCL error " + std::to_string(status));
  }
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
