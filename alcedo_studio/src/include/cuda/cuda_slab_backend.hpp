//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <utility>

#include <cuda_runtime.h>

#include "cuda/cuda_check.hpp"

namespace alcedo::cuda {

/**
 * @brief CUDA slab factory for @ref TransientBufferArena. Independent of edit runtime.
 *
 * CreateSlab calls cudaMalloc. Slab destructor calls cudaFree. Not thread-safe.
 */
struct CudaSlabBackend {
  class Slab {
   public:
    Slab() = default;
    Slab(void* ptr, std::size_t bytes) : ptr_(ptr), bytes_(bytes) {}

    Slab(const Slab&)            = delete;
    auto operator=(const Slab&) -> Slab& = delete;

    Slab(Slab&& other) noexcept : ptr_(other.ptr_), bytes_(other.bytes_) {
      other.ptr_   = nullptr;
      other.bytes_ = 0;
    }

    auto operator=(Slab&& other) noexcept -> Slab& {
      if (this != &other) {
        Reset();
        ptr_         = other.ptr_;
        bytes_       = other.bytes_;
        other.ptr_   = nullptr;
        other.bytes_ = 0;
      }
      return *this;
    }

    ~Slab() { Reset(); }

    void Reset() noexcept {
      if (ptr_ != nullptr) {
        ::cudaFree(ptr_);
        ptr_ = nullptr;
      }
      bytes_ = 0;
    }

    [[nodiscard]] auto DevicePointer() const -> void* { return ptr_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }

   private:
    void*       ptr_   = nullptr;
    std::size_t bytes_ = 0;
  };

  [[nodiscard]] auto CreateSlab(std::size_t bytes) -> Slab {
    if (bytes == 0) {
      return {};
    }
    void* ptr = nullptr;
    CheckCuda(::cudaMalloc(&ptr, bytes), "CudaSlabBackend::CreateSlab");
    return Slab{ptr, bytes};
  }
};

}  // namespace alcedo::cuda
