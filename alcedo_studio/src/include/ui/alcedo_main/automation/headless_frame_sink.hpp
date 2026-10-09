//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "ui/edit_viewer/frame_sink.hpp"

#ifdef HAVE_OPENCL
#include <CL/cl.h>
#endif

namespace alcedo::automation {

/// Presentation target of the headless host. It replaces the QML viewport so that the editor
/// session can present frames and reach Interactive without a window.
///
/// CUDA writes the frame into a host-visible RGBA32F buffer. OpenCL writes into an OpenCL
/// RGBA32F image of the active OpenCL context. Metal hands over its texture through
/// `SubmitMetalFrame`, which the sink does not keep. Render workers call the sink; the GUI
/// thread reads the size and the frame count. A mutex guards the buffers.
class HeadlessFrameSink final : public IFrameSink {
 public:
  HeadlessFrameSink() = default;
  ~HeadlessFrameSink() override;

  HeadlessFrameSink(const HeadlessFrameSink&)                    = delete;
  auto operator=(const HeadlessFrameSink&) -> HeadlessFrameSink& = delete;

  void EnsureSize(int width, int height) override;
  /// `CudaDevice` and `HostVisible` get the host buffer. `OpenClDevice` gets the OpenCL image.
  /// An empty mapping means that the sink cannot serve the domain; the renderer then fails with
  /// its own error.
  auto MapResourceForWrite(FrameMemoryDomain preferred_domain = FrameMemoryDomain::CudaDevice)
      -> FrameWriteMapping override;
  void               UnmapResource() override {}
  void               NotifyFrameReady(const FrameCompletionSubmission& submission) override;

  [[nodiscard]] auto GetWidth() const -> int override;
  [[nodiscard]] auto GetHeight() const -> int override;

  /// Number of frames that the renderer completed into this sink.
  [[nodiscard]] auto ready_frame_count() const -> std::uint64_t {
    return ready_frame_count_.load(std::memory_order_acquire);
  }

 private:
  void               ReleaseOpenClImage();

  mutable std::mutex mutex_;
  int                width_  = 0;
  int                height_ = 0;
  std::vector<float> host_pixels_;
#ifdef HAVE_OPENCL
  cl_mem opencl_image_ = nullptr;
#endif
  std::atomic<std::uint64_t> ready_frame_count_ = 0;
};

}  // namespace alcedo::automation
