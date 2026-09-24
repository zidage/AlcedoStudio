//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <memory>

#include "edit/operators/op_base.hpp"
#include "image/gpu_backend.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
class ImageBuffer;
class IFrameSink;

class GPUPipelineImpl {
 public:
  virtual ~GPUPipelineImpl() = default;

  virtual void SetInputImage(std::shared_ptr<ImageBuffer> input_image) = 0;
  virtual void SetParams(OperatorParams& params)                        = 0;
  virtual void SetFrameSink(IFrameSink* frame_sink)                    = 0;
  virtual void SetBoundFrameSubmission(const FrameCompletionSubmission& /*submission*/) {}
  virtual void Execute(std::shared_ptr<ImageBuffer> output)            = 0;
  virtual void ReleaseScratchBuffers() {}
  virtual void ReleaseResources()                                      = 0;
  [[nodiscard]] virtual auto DebugGetAllocatedScratchBytes() const -> size_t { return 0; }
};

class GPUPipelineWrapper {
 public:
  GPUPipelineWrapper();
  explicit GPUPipelineWrapper(GpuBackendKind backend);
  ~GPUPipelineWrapper();

  void SetBackend(GpuBackendKind backend);
  [[nodiscard]] auto Backend() const -> GpuBackendKind;

  void SetInputImage(std::shared_ptr<ImageBuffer> input_image);

  void SetParams(OperatorParams& params);

  void SetFrameSink(IFrameSink* frame_sink);

  void SetBoundFrameSubmission(const FrameCompletionSubmission& submission);

  void Execute(std::shared_ptr<ImageBuffer> output);

  [[nodiscard]] auto HasAcceleratedBackend() const -> bool;

  // Frees persistent GPU allocations used by the pipeline (scratch buffers, LUTs, etc.).
  // Intended for batch export to avoid holding large VRAM allocations across many pipelines.
  void ReleaseResources();

  // Frees transient scratch buffers while keeping immutable pipeline state intact.
  // Used by preview mode transitions to drop stale high-water allocations without
  // rebuilding LUTs or other long-lived GPU state.
  void ReleaseScratchBuffers();

  [[nodiscard]] auto DebugGetAllocatedScratchBytes() const -> size_t;

 private:
  std::unique_ptr<GPUPipelineImpl> impl_;
  GpuBackendKind                   backend_ = GpuBackendKind::None;
};
}  // namespace alcedo
