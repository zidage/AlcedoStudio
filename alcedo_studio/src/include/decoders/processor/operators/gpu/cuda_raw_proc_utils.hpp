//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#define CUDA_CHECK(call)                                                                         \
  do {                                                                                           \
    cudaError_t err = call;                                                                      \
    if (err != cudaSuccess) {                                                                    \
      fprintf(stderr, "CUDA Error: %s at %s:%d\n", cudaGetErrorString(err), __FILE__, __LINE__); \
      exit(EXIT_FAILURE);                                                                        \
    }                                                                                            \
  } while (0)
