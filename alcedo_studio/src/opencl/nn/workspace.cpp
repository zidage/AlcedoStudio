//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/nn/workspace.hpp"

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

SubBuffer::SubBuffer(const WorkspaceSlice& slice) {
  if (slice.empty()) {
    return;
  }
  if (slice.parent == nullptr) {
    throw std::runtime_error("SubBuffer: null parent");
  }
  cl_buffer_region region{};
  region.origin = slice.byte_offset;
  region.size   = slice.byte_size;
  cl_int error  = CL_SUCCESS;
  buffer_ =
      clCreateSubBuffer(slice.parent, CL_MEM_READ_WRITE, CL_BUFFER_CREATE_TYPE_REGION, &region, &error);
  CheckOpenCl(error, "SubBuffer::clCreateSubBuffer");
  if (buffer_ == nullptr) {
    throw std::runtime_error("SubBuffer::clCreateSubBuffer returned null");
  }
  NoteOpenClCreateSubBuffer();
}

void SubBuffer::Reset() noexcept {
  if (buffer_ != nullptr) {
    clReleaseMemObject(buffer_);
    NoteOpenClReleaseMemObject();
    buffer_ = nullptr;
  }
}

void WorkspacePool::Reserve(std::size_t bytes) {
  if (bytes <= capacity_) {
    return;
  }
  if (offset_ != 0) {
    throw std::runtime_error(
        "WorkspacePool::Reserve: cannot grow while allocations are live; "
        "Reset() first or Reserve peak size before Allocate");
  }

  auto&  context = CheckedContext();
  cl_int error   = CL_SUCCESS;
  cl_mem new_buf =
      clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, bytes, nullptr, &error);
  CheckOpenCl(error, "WorkspacePool::Reserve clCreateBuffer");
  if (new_buf == nullptr) {
    throw std::runtime_error("WorkspacePool::Reserve: clCreateBuffer returned null");
  }
  NoteOpenClCreateBuffer();

  if (buffer_ != nullptr) {
    clReleaseMemObject(buffer_);
    NoteOpenClReleaseMemObject();
  }
  buffer_ = new_buf;
  capacity_ = bytes;
  ++allocation_generation_;
}

auto WorkspacePool::Allocate(std::size_t bytes, std::size_t alignment) -> WorkspaceSlice {
  if (bytes == 0) {
    return {};
  }
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    throw std::runtime_error("WorkspacePool::Allocate: alignment must be a power of two");
  }

  const std::size_t aligned_offset = AlignUp(offset_, alignment);
  const std::size_t end            = aligned_offset + bytes;

  if (end > capacity_) {
    if (offset_ != 0) {
      throw std::runtime_error(
          "WorkspacePool::Allocate: insufficient capacity with live allocations; "
          "call Reserve() with the peak workspace size before forward");
    }
    std::size_t new_cap = capacity_ == 0 ? end : capacity_;
    while (new_cap < end) {
      const std::size_t doubled = new_cap > (std::size_t{1} << 62) ? end : new_cap * 2;
      new_cap                   = doubled < end ? end : doubled;
    }
    Reserve(new_cap);
  }

  WorkspaceSlice slice;
  slice.parent      = buffer_;
  slice.byte_offset = aligned_offset;
  slice.byte_size   = bytes;
  offset_           = end;
  return slice;
}

auto WorkspacePool::AllocateNhwc4(int batch, int height, int width, int logical_channels,
                                  std::size_t alignment)
    -> std::pair<WorkspaceSlice, Nhwc4TensorView> {
  return AllocateNhwc4Blocked(batch, height, width, logical_channels,
                              ChannelBlocks(logical_channels), alignment);
}

auto WorkspacePool::AllocateNhwc4Blocked(int batch, int height, int width, int logical_channels,
                                         int channel_blocks, std::size_t alignment)
    -> std::pair<WorkspaceSlice, Nhwc4TensorView> {
  if (batch <= 0 || height <= 0 || width <= 0 || logical_channels < 0 || channel_blocks <= 0) {
    throw std::runtime_error("WorkspacePool::AllocateNhwc4Blocked: invalid geometry");
  }
  if (channel_blocks < ChannelBlocks(logical_channels)) {
    throw std::runtime_error(
        "WorkspacePool::AllocateNhwc4Blocked: channel_blocks smaller than logical requirement");
  }
  const std::size_t floats = static_cast<std::size_t>(batch) * static_cast<std::size_t>(height) *
                             static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(channel_blocks) * 4u;
  const std::size_t bytes = floats * sizeof(float);
  WorkspaceSlice    slice = Allocate(bytes, alignment);

  Nhwc4TensorView view;
  view.buffer           = slice.parent;
  view.batch            = batch;
  view.height           = height;
  view.width            = width;
  view.logical_channels = logical_channels;
  view.channel_blocks   = channel_blocks;
  view.byte_offset      = slice.byte_offset;
  return {slice, view};
}

void WorkspacePool::FreeDevice() noexcept {
  if (buffer_ != nullptr) {
    clReleaseMemObject(buffer_);
    NoteOpenClReleaseMemObject();
    buffer_ = nullptr;
  }
  capacity_ = 0;
  offset_   = 0;
}

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
