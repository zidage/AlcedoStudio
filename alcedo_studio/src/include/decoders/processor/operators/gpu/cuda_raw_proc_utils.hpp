//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdlib>

#include "utils/logging/log.hpp"

#define CUDA_CHECK(call)                                                                 \
  do {                                                                                   \
    cudaError_t err = call;                                                              \
    if (err != cudaSuccess) {                                                            \
      ALCEDO_LOG_ERROR(Gpu) << "CUDA error " << cudaGetErrorString(err) << " in " #call; \
      std::exit(EXIT_FAILURE);                                                           \
    }                                                                                    \
  } while (0)
