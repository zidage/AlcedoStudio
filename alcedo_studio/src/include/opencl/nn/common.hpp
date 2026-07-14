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

namespace alcedo::opencl::nn {

inline void CheckOpenCl(cl_int status, const char* what) {
  if (status != CL_SUCCESS) {
    throw std::runtime_error(std::string(what) + ": OpenCL error " + std::to_string(status));
  }
}

// Ceiling division for channel-block counts: (c + 3) / 4.
[[nodiscard]] inline auto ChannelBlocks(int logical_channels) -> int {
  if (logical_channels < 0) {
    throw std::runtime_error("ChannelBlocks: logical_channels must be non-negative");
  }
  return (logical_channels + 3) / 4;
}

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
