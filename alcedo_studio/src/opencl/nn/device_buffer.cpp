//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/nn/device_buffer.hpp"

#include <stdexcept>
#include <string>

#include "opencl/opencl_api_counters.hpp"

namespace alcedo::opencl::nn {
namespace {

auto CheckedContext() -> OpenClContext& {
  auto& context = OpenClContext::Instance();
  context.Initialize();
  return context;
}

}  // namespace

auto DeviceBuffer::ResolveQueue(cl_command_queue queue) -> cl_command_queue {
  if (queue != nullptr) {
    return queue;
  }
  return CheckedContext().Queue();
}

DeviceBuffer::DeviceBuffer(std::size_t byte_count) : byte_capacity_(byte_count) {
  if (byte_count == 0) {
    return;
  }
  auto&  context = CheckedContext();
  cl_int error   = CL_SUCCESS;
  buffer_ = clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, byte_count, nullptr, &error);
  CheckOpenCl(error, "DeviceBuffer::clCreateBuffer");
  if (buffer_ == nullptr) {
    throw std::runtime_error("DeviceBuffer::clCreateBuffer returned null");
  }
  NoteOpenClCreateBuffer();
}

void DeviceBuffer::Reset() noexcept {
  if (buffer_ != nullptr) {
    clReleaseMemObject(buffer_);
    NoteOpenClReleaseMemObject();
    buffer_ = nullptr;
  }
  byte_capacity_ = 0;
}

void DeviceBuffer::UploadBytes(const void* host, std::size_t byte_count, cl_command_queue queue,
                               bool blocking) {
  if (byte_count > byte_capacity_) {
    throw std::runtime_error("DeviceBuffer::UploadBytes: size exceeds capacity");
  }
  if (byte_count == 0) {
    return;
  }
  if (host == nullptr) {
    throw std::runtime_error("DeviceBuffer::UploadBytes: null host pointer");
  }
  if (buffer_ == nullptr) {
    throw std::runtime_error("DeviceBuffer::UploadBytes: empty buffer");
  }
  CheckOpenCl(clEnqueueWriteBuffer(ResolveQueue(queue), buffer_, blocking ? CL_TRUE : CL_FALSE, 0,
                                   byte_count, host, 0, nullptr, nullptr),
              "DeviceBuffer::UploadBytes");
  NoteOpenClH2DBytes(static_cast<std::uint64_t>(byte_count));
}

void DeviceBuffer::DownloadBytes(void* host, std::size_t byte_count, cl_command_queue queue,
                                 bool blocking) const {
  if (byte_count > byte_capacity_) {
    throw std::runtime_error("DeviceBuffer::DownloadBytes: size exceeds capacity");
  }
  if (byte_count == 0) {
    return;
  }
  if (host == nullptr) {
    throw std::runtime_error("DeviceBuffer::DownloadBytes: null host pointer");
  }
  if (buffer_ == nullptr) {
    throw std::runtime_error("DeviceBuffer::DownloadBytes: empty buffer");
  }
  CheckOpenCl(clEnqueueReadBuffer(ResolveQueue(queue), buffer_, blocking ? CL_TRUE : CL_FALSE, 0,
                                  byte_count, host, 0, nullptr, nullptr),
              "DeviceBuffer::DownloadBytes");
  NoteOpenClD2HBytes(static_cast<std::uint64_t>(byte_count));
}

void DeviceBuffer::FillZero(cl_command_queue queue, bool blocking) {
  if (byte_capacity_ == 0 || buffer_ == nullptr) {
    return;
  }
  const cl_uint pattern = 0;
  CheckOpenCl(clEnqueueFillBuffer(ResolveQueue(queue), buffer_, &pattern, sizeof(pattern), 0,
                                  byte_capacity_, 0, nullptr, nullptr),
              "DeviceBuffer::FillZero");
  if (blocking) {
    CheckOpenCl(clFinish(ResolveQueue(queue)), "DeviceBuffer::FillZero clFinish");
    NoteOpenClQueueFinish();
  }
}

void DeviceBuffer::EnsureBytes(const std::size_t bytes) {
  if (bytes <= byte_capacity_) {
    return;
  }
  DeviceBuffer grown(bytes);
  *this = std::move(grown);
}

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
