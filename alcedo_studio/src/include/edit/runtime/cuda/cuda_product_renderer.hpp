//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "edit/geometry/render_request.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

class CudaRenderDevice;
class ImageBuffer;
class PipelineDocument;

/**
 * @brief Product adapter from the existing scheduler inputs to the CUDA DAG renderer.
 *
 * Owns one device workspace per loaded pipeline so node results and allocations survive
 * consecutive scheduler frames. Failures are reported and propagated; this adapter never calls
 * the old CPU image-processing stages.
 */
class CudaProductRenderer {
 public:
  explicit CudaProductRenderer(std::shared_ptr<PipelineDocument> document);
  ~CudaProductRenderer();

  CudaProductRenderer(const CudaProductRenderer&)                                  = delete;
  auto               operator=(const CudaProductRenderer&) -> CudaProductRenderer& = delete;

  void               SetDocument(std::shared_ptr<PipelineDocument> document);

  [[nodiscard]] auto Render(const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                            const RenderRequest& request, IFrameSink* sink,
                            const FrameCompletionSubmission& submission, bool require_host_output)
      -> std::shared_ptr<ImageBuffer>;

 private:
  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<CudaRenderDevice> device_;
};

}  // namespace alcedo
