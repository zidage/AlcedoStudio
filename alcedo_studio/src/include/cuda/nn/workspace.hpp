//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

#include "cuda/cuda_slab_backend.hpp"
#include "cuda/nn/tensor.hpp"
#include "gpu/transient_buffer_arena.hpp"
#include "gpu/transient_buffer_scope.hpp"

namespace alcedo::cuda::nn {

/**
 * @brief CNN-facing bump workspace. Same slab policy as edit TransientBufferArena.
 *
 * AllocateTensor / AllocateFloats stay here because they need DeviceTensor.
 * Not thread-safe. One pool per stream or serialize access.
 */
class WorkspacePool : public TransientBufferArena<cuda::CudaSlabBackend> {
 public:
  using Base = TransientBufferArena<cuda::CudaSlabBackend>;
  using Base::Base;

  [[nodiscard]] auto AllocateFloats(std::size_t count,
                                    std::size_t alignment = kDefaultAlignment) -> float* {
    return Allocate<float>(count, alignment);
  }

  [[nodiscard]] auto AllocateTensor(std::initializer_list<std::int64_t> dims,
                                    std::size_t alignment = kDefaultAlignment) -> DeviceTensor {
    return AllocateTensor(dims.begin(), static_cast<int>(dims.size()), alignment);
  }

  [[nodiscard]] auto AllocateTensor(const std::vector<std::int64_t>& dims,
                                    std::size_t alignment = kDefaultAlignment) -> DeviceTensor {
    return AllocateTensor(dims.data(), static_cast<int>(dims.size()), alignment);
  }

 private:
  [[nodiscard]] auto AllocateTensor(const std::int64_t* dims, int rank,
                                    std::size_t alignment) -> DeviceTensor {
    std::int64_t numel = 1;
    for (int i = 0; i < rank; ++i) {
      if (dims[i] < 0) {
        throw std::runtime_error("WorkspacePool::AllocateTensor: negative dimension");
      }
      numel *= dims[i];
    }
    float* ptr = AllocateFloats(static_cast<std::size_t>(numel), alignment);
    return DeviceTensor::Contiguous(ptr, dims, rank);
  }
};

using WorkspaceScope = TransientBufferScope<cuda::CudaSlabBackend>;

}  // namespace alcedo::cuda::nn
