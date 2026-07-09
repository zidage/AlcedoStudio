//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/common.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Owning device allocation (RAII cudaMalloc / cudaFree).
//
// Primary use: long-lived weight buffers and test fixtures. Intermediate CNN
// activations should prefer WorkspacePool (bump allocator) so steady-state
// forward does not call cudaMalloc.
//
// Move-only. Element type is typically float for the demosaicnet milestone.
template <typename T>
class DeviceBuffer {
 public:
  DeviceBuffer() = default;

  explicit DeviceBuffer(std::size_t count) : count_(count) {
    if (count_ == 0) {
      return;
    }
    void* raw = nullptr;
    CheckCuda(::cudaMalloc(&raw, sizeof(T) * count_), "DeviceBuffer::cudaMalloc");
    ptr_ = static_cast<T*>(raw);
  }

  DeviceBuffer(const DeviceBuffer&)            = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;

  DeviceBuffer(DeviceBuffer&& other) noexcept : ptr_(other.ptr_), count_(other.count_) {
    other.ptr_   = nullptr;
    other.count_ = 0;
  }

  auto operator=(DeviceBuffer&& other) noexcept -> DeviceBuffer& {
    if (this != &other) {
      Reset();
      ptr_         = other.ptr_;
      count_       = other.count_;
      other.ptr_   = nullptr;
      other.count_ = 0;
    }
    return *this;
  }

  ~DeviceBuffer() { Reset(); }

  [[nodiscard]] auto get() -> T* { return ptr_; }
  [[nodiscard]] auto get() const -> const T* { return ptr_; }
  [[nodiscard]] auto data() -> T* { return ptr_; }
  [[nodiscard]] auto data() const -> const T* { return ptr_; }

  [[nodiscard]] auto size() const -> std::size_t { return count_; }
  [[nodiscard]] auto bytes() const -> std::size_t { return count_ * sizeof(T); }
  [[nodiscard]] auto empty() const -> bool { return count_ == 0 || ptr_ == nullptr; }

  // Synchronous (default stream) or async upload. Count must match size().
  void Upload(const T* host, std::size_t count, cudaStream_t stream = nullptr) {
    if (count != count_) {
      throw std::runtime_error("DeviceBuffer::Upload size mismatch");
    }
    if (count_ == 0) {
      return;
    }
    if (host == nullptr) {
      throw std::runtime_error("DeviceBuffer::Upload null host pointer");
    }
    const auto nbytes = sizeof(T) * count_;
    if (stream == nullptr) {
      CheckCuda(::cudaMemcpy(ptr_, host, nbytes, cudaMemcpyHostToDevice), "DeviceBuffer::Upload");
    } else {
      CheckCuda(::cudaMemcpyAsync(ptr_, host, nbytes, cudaMemcpyHostToDevice, stream),
                "DeviceBuffer::Upload");
    }
  }

  void Upload(const std::vector<T>& host, cudaStream_t stream = nullptr) {
    Upload(host.data(), host.size(), stream);
  }

  // Synchronous (default stream) or async download. Count must match size().
  void Download(T* host, std::size_t count, cudaStream_t stream = nullptr) const {
    if (count != count_) {
      throw std::runtime_error("DeviceBuffer::Download size mismatch");
    }
    if (count_ == 0) {
      return;
    }
    if (host == nullptr) {
      throw std::runtime_error("DeviceBuffer::Download null host pointer");
    }
    const auto nbytes = sizeof(T) * count_;
    if (stream == nullptr) {
      CheckCuda(::cudaMemcpy(host, ptr_, nbytes, cudaMemcpyDeviceToHost), "DeviceBuffer::Download");
    } else {
      CheckCuda(::cudaMemcpyAsync(host, ptr_, nbytes, cudaMemcpyDeviceToHost, stream),
                "DeviceBuffer::Download");
    }
  }

  [[nodiscard]] auto Download(cudaStream_t stream = nullptr) const -> std::vector<T> {
    std::vector<T> host(count_);
    if (count_ > 0) {
      Download(host.data(), count_, stream);
    }
    return host;
  }

  // Zero the buffer (sync or async).
  void FillZero(cudaStream_t stream = nullptr) {
    if (count_ == 0) {
      return;
    }
    if (stream == nullptr) {
      CheckCuda(::cudaMemset(ptr_, 0, bytes()), "DeviceBuffer::FillZero");
    } else {
      CheckCuda(::cudaMemsetAsync(ptr_, 0, bytes(), stream), "DeviceBuffer::FillZero");
    }
  }

  // Contiguous DeviceTensor view over this buffer. Numel must match size().
  [[nodiscard]] auto AsTensor(std::initializer_list<std::int64_t> dims) -> DeviceTensor {
    return MakeTensor(dims.begin(), static_cast<int>(dims.size()));
  }

  [[nodiscard]] auto AsTensor(const std::vector<std::int64_t>& dims) -> DeviceTensor {
    return MakeTensor(dims.data(), static_cast<int>(dims.size()));
  }

  // Release device memory early (also called from destructor).
  void Reset() noexcept {
    if (ptr_ != nullptr) {
      ::cudaFree(ptr_);
      ptr_ = nullptr;
    }
    count_ = 0;
  }

 private:
  [[nodiscard]] auto MakeTensor(const std::int64_t* dims, int rank) -> DeviceTensor {
    auto tensor = DeviceTensor::Contiguous(ptr_, dims, rank);
    if (static_cast<std::size_t>(tensor.Numel()) != count_) {
      throw std::runtime_error("DeviceBuffer::AsTensor: shape numel does not match buffer size");
    }
    return tensor;
  }

  T*          ptr_   = nullptr;
  std::size_t count_ = 0;
};

// f32 is the only activation/weight dtype for the demosaicnet milestone.
using DeviceBufferF32 = DeviceBuffer<float>;

}  // namespace alcedo::cuda::nn
