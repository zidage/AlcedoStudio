//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>

#include "opencl/opencl_context.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo::scope::opencl_detail {

struct OpenClLinearImageResource {
  cl_mem           buffer        = nullptr;
  size_t           row_bytes     = 0;
  int              width         = 0;
  int              height        = 0;
  FramePixelFormat format        = FramePixelFormat::RGBA32F;
  bool             owns_memory   = false;
  std::uintptr_t   native_object = 0;

  ~OpenClLinearImageResource() {
    if (owns_memory && buffer != nullptr) {
      clReleaseMemObject(buffer);
      buffer = nullptr;
    }
  }
};

struct OpenClImageResource {
  cl_mem           image         = nullptr;
  int              width         = 0;
  int              height        = 0;
  FramePixelFormat format        = FramePixelFormat::RGBA32F;
  bool             owns_memory   = false;
  std::uintptr_t   native_object = 0;

  ~OpenClImageResource() {
    if (owns_memory && image != nullptr) {
      clReleaseMemObject(image);
      image = nullptr;
    }
  }
};

struct OpenClEventSignalResource {
  cl_event event = nullptr;

  ~OpenClEventSignalResource() {
    if (event != nullptr) {
      clReleaseEvent(event);
      event = nullptr;
    }
  }
};

struct OpenClBufferResource {
  cl_mem buffer      = nullptr;
  size_t size_bytes  = 0;
  bool   owns_memory = false;

  ~OpenClBufferResource() {
    if (owns_memory && buffer != nullptr) {
      clReleaseMemObject(buffer);
      buffer = nullptr;
    }
  }
};

}  // namespace alcedo::scope::opencl_detail

#endif
