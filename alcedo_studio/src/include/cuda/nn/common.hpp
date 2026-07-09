//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <stdexcept>
#include <string>

namespace alcedo::cuda::nn {

inline void CheckCuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
  }
}

// Prefer enough resident threads for latency hiding, then fall back to grid-stride.
// See: NVIDIA "CUDA Pro Tip: Write Flexible Kernels with Grid-Stride Loops".
inline auto ChooseGridSize(std::int64_t work_items, int block_size) -> int {
  if (work_items <= 0) {
    return 0;
  }
  constexpr int kMaxBlocks = 4096;
  const auto   needed =
      static_cast<int>((work_items + static_cast<std::int64_t>(block_size) - 1) /
                       static_cast<std::int64_t>(block_size));
  return needed < kMaxBlocks ? needed : kMaxBlocks;
}

inline auto ResolveStream(cudaStream_t stream) -> cudaStream_t {
  return stream;
}

}  // namespace alcedo::cuda::nn
