//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/headless_frame_sink.hpp"

#include <QDebug>
#include <cstddef>

#ifdef HAVE_OPENCL
#include "opencl/opencl_context.hpp"
#endif

namespace alcedo::automation {
namespace {

constexpr std::size_t kChannels = 4;

}  // namespace

HeadlessFrameSink::~HeadlessFrameSink() {
  std::lock_guard lock(mutex_);
  ReleaseOpenClImage();
}

void HeadlessFrameSink::EnsureSize(int width, int height) {
  std::lock_guard lock(mutex_);
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) {
    return;
  }
  width_  = width;
  height_ = height;
  host_pixels_.assign(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels, 0.0F);
  ReleaseOpenClImage();
}

auto HeadlessFrameSink::MapResourceForWrite(FrameMemoryDomain preferred_domain)
    -> FrameWriteMapping {
  std::lock_guard   lock(mutex_);
  FrameWriteMapping mapping;
  if (width_ <= 0 || height_ <= 0) {
    return mapping;
  }
  mapping.pixel_format = FramePixelFormat::RGBA32F;

  if (preferred_domain == FrameMemoryDomain::OpenClDevice) {
#ifdef HAVE_OPENCL
    if (opencl_image_ == nullptr) {
      const cl_image_format format{CL_RGBA, CL_FLOAT};
      cl_image_desc         desc{};
      desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
      desc.image_width  = static_cast<std::size_t>(width_);
      desc.image_height = static_cast<std::size_t>(height_);
      cl_int status     = CL_SUCCESS;
      opencl_image_ = clCreateImage(OpenClContext::Instance().Context(), CL_MEM_READ_WRITE, &format,
                                    &desc, nullptr, &status);
      if (status != CL_SUCCESS || opencl_image_ == nullptr) {
        qWarning("HeadlessFrameSink: clCreateImage(%dx%d) failed with %d", width_, height_, status);
        opencl_image_ = nullptr;
        return mapping;
      }
    }
    mapping.data          = reinterpret_cast<void*>(opencl_image_);
    mapping.memory_domain = FrameMemoryDomain::OpenClDevice;
    mapping.target_type   = FrameWriteTargetType::OpenClImage;
    return mapping;
#else
    return mapping;
#endif
  }

  mapping.data          = host_pixels_.data();
  mapping.row_bytes     = static_cast<std::size_t>(width_) * kChannels * sizeof(float);
  mapping.memory_domain = FrameMemoryDomain::HostVisible;
  mapping.target_type   = FrameWriteTargetType::LinearBuffer;
  return mapping;
}

void HeadlessFrameSink::NotifyFrameReady(const FrameCompletionSubmission& /*submission*/) {
  ready_frame_count_.fetch_add(1, std::memory_order_acq_rel);
}

auto HeadlessFrameSink::GetWidth() const -> int {
  std::lock_guard lock(mutex_);
  return width_;
}

auto HeadlessFrameSink::GetHeight() const -> int {
  std::lock_guard lock(mutex_);
  return height_;
}

void HeadlessFrameSink::ReleaseOpenClImage() {
#ifdef HAVE_OPENCL
  if (opencl_image_ != nullptr) {
    clReleaseMemObject(opencl_image_);
    opencl_image_ = nullptr;
  }
#endif
}

}  // namespace alcedo::automation
