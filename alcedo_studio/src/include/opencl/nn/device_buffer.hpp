//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "opencl/nn/common.hpp"
#include "opencl/opencl_context.hpp"

namespace alcedo::opencl::nn {

// Move-only owning cl_mem buffer with explicit byte capacity.
//
// Primary use: long-lived weight buffers, fixed activation slots, and fixtures.
// DemosaicNet product activations use two dedicated grow-only DeviceBuffers
// (ActivationSlots); generic WorkspacePool remains for primitives/tests.
//
// Element type is FP32 for the DemosaicNet milestone.
class DeviceBuffer {
 public:
  DeviceBuffer() = default;

  // Allocates `byte_count` device bytes (may be 0 → empty).
  explicit DeviceBuffer(std::size_t byte_count);

  // Allocates storage for `element_count` floats.
  static auto Floats(std::size_t element_count) -> DeviceBuffer {
    return DeviceBuffer(element_count * sizeof(float));
  }

  DeviceBuffer(const DeviceBuffer&)            = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;

  DeviceBuffer(DeviceBuffer&& other) noexcept
      : buffer_(other.buffer_), byte_capacity_(other.byte_capacity_) {
    other.buffer_        = nullptr;
    other.byte_capacity_ = 0;
  }

  auto operator=(DeviceBuffer&& other) noexcept -> DeviceBuffer& {
    if (this != &other) {
      Reset();
      buffer_              = other.buffer_;
      byte_capacity_       = other.byte_capacity_;
      other.buffer_        = nullptr;
      other.byte_capacity_ = 0;
    }
    return *this;
  }

  ~DeviceBuffer() { Reset(); }

  [[nodiscard]] auto get() const noexcept -> cl_mem { return buffer_; }
  [[nodiscard]] auto buffer() const noexcept -> cl_mem { return buffer_; }
  [[nodiscard]] auto byte_capacity() const noexcept -> std::size_t { return byte_capacity_; }
  [[nodiscard]] auto float_capacity() const noexcept -> std::size_t {
    return byte_capacity_ / sizeof(float);
  }
  [[nodiscard]] auto empty() const noexcept -> bool {
    return buffer_ == nullptr || byte_capacity_ == 0;
  }

  // Blocking host→device copy of `byte_count` bytes (must fit capacity).
  void UploadBytes(const void* host, std::size_t byte_count, cl_command_queue queue = nullptr,
                   bool blocking = true);

  void UploadFloats(const float* host, std::size_t count, cl_command_queue queue = nullptr,
                    bool blocking = true) {
    UploadBytes(host, count * sizeof(float), queue, blocking);
  }

  void UploadFloats(const std::vector<float>& host, cl_command_queue queue = nullptr,
                    bool blocking = true) {
    UploadFloats(host.data(), host.size(), queue, blocking);
  }

  // Blocking device→host copy of `byte_count` bytes (must fit capacity).
  void DownloadBytes(void* host, std::size_t byte_count, cl_command_queue queue = nullptr,
                     bool blocking = true) const;

  void DownloadFloats(float* host, std::size_t count, cl_command_queue queue = nullptr,
                      bool blocking = true) const {
    DownloadBytes(host, count * sizeof(float), queue, blocking);
  }

  [[nodiscard]] auto DownloadFloats(std::size_t count, cl_command_queue queue = nullptr) const
      -> std::vector<float> {
    std::vector<float> host(count);
    if (count > 0) {
      DownloadFloats(host.data(), count, queue, true);
    }
    return host;
  }

  // Zero the allocated region.
  void FillZero(cl_command_queue queue = nullptr, bool blocking = true);

  // Grow-only capacity ensure. No-op when `bytes <= byte_capacity()`.
  // Reallocates (and releases the previous object) only when capacity must grow.
  void EnsureBytes(std::size_t bytes);

  void Reset() noexcept;

 private:
  static auto ResolveQueue(cl_command_queue queue) -> cl_command_queue;

  cl_mem      buffer_        = nullptr;
  std::size_t byte_capacity_ = 0;
};

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
