//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/pipeline/pipeline_executor.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "image/image_buffer.hpp"
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#endif
#ifdef HAVE_CUDA
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#endif
#ifdef HAVE_METAL
#include "edit/runtime/metal/metal_renderer.hpp"
#endif
#ifdef HAVE_OPENCL
#include "edit/runtime/opencl/opencl_renderer.hpp"
#endif

namespace alcedo {

namespace {

#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
/** @brief Reuse the renderer bound to this document; propagate all render failures to the caller. */
template <class ProductRenderer>
auto ApplyGpuDagProduct(std::shared_ptr<ProductRenderer>&            renderer,
                        const std::shared_ptr<PipelineDocument>&     document,
                        const std::shared_ptr<ImageBuffer>&          input,
                        const PipelineApplyRequest&                  request)
    -> std::shared_ptr<ImageBuffer> {
  if (!renderer) {
    renderer = std::make_shared<ProductRenderer>(document);
  }
  return renderer->Render(input, request);
}
#endif

}  // namespace

PipelineExecutor::PipelineExecutor()
    : resolved_accelerator_backend_(alcedo::ResolveAcceleratorBackend(accelerator_preference_)) {}

auto PipelineExecutor::Apply(std::shared_ptr<ImageBuffer> input,
                             const PipelineApplyRequest&  request) -> std::shared_ptr<ImageBuffer> {
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  if (!pipeline_document_) {
    throw std::runtime_error(
        "PipelineExecutor: product rendering requires a bound PipelineDocument");
  }
#ifdef HAVE_CUDA
  if (resolved_accelerator_backend_ == GpuBackendKind::CUDA) {
    return ApplyGpuDagProduct(cuda_product_renderer_, pipeline_document_, input, request);
  }
#endif
#ifdef HAVE_METAL
  if (resolved_accelerator_backend_ == GpuBackendKind::Metal) {
    return ApplyGpuDagProduct(metal_product_renderer_, pipeline_document_, input, request);
  }
#endif
#ifdef HAVE_OPENCL
  if (resolved_accelerator_backend_ == GpuBackendKind::OpenCL) {
    return ApplyGpuDagProduct(opencl_product_renderer_, pipeline_document_, input, request);
  }
#endif
#endif
  (void)input;
  (void)request;
  throw std::runtime_error("PipelineExecutor: product rendering requires a supported GPU backend");
}

void PipelineExecutor::SetPipelineDocument(std::shared_ptr<PipelineDocument> document) {
  if (!document) {
    throw std::invalid_argument("PipelineExecutor: PipelineDocument is null");
  }
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  pipeline_document_ = std::move(document);

#ifdef HAVE_CUDA
  if (cuda_product_renderer_) {
    cuda_product_renderer_->SetDocument(pipeline_document_);
  }
#endif
#ifdef HAVE_METAL
  if (metal_product_renderer_) {
    metal_product_renderer_->SetDocument(pipeline_document_);
  }
#endif
#ifdef HAVE_OPENCL
  if (opencl_product_renderer_) {
    opencl_product_renderer_->SetDocument(pipeline_document_);
  }
#endif
#else
  (void)document;
#endif
}

auto PipelineExecutor::HasGpuDagDocument() const -> bool {
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  return static_cast<bool>(pipeline_document_);
#else
  return false;
#endif
}

auto PipelineExecutor::GpuDagDocument() const -> std::shared_ptr<PipelineDocument> {
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  return pipeline_document_;
#else
  return nullptr;
#endif
}

void PipelineExecutor::SetAcceleratorBackendPreference(
    const AcceleratorBackendPreference preference) {
  // Resolve first: an unavailable backend throws and leaves the prior selection in place.
  const auto resolved           = alcedo::ResolveAcceleratorBackend(preference);
  accelerator_preference_       = preference;
  resolved_accelerator_backend_ = resolved;
}

auto PipelineExecutor::GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> {
  if (!frame_sink_) {
    return std::nullopt;
  }
  return frame_sink_->GetViewportRenderRegion();
}

void PipelineExecutor::ClearAllIntermediateBuffers() {
#ifdef HAVE_CUDA
  if (cuda_product_renderer_) {
    cuda_product_renderer_->ReleaseSessionCaches();
  }
#endif
#ifdef HAVE_METAL
  if (metal_product_renderer_) {
    metal_product_renderer_->ReleaseSessionCaches();
  }
#endif
#ifdef HAVE_OPENCL
  if (opencl_product_renderer_) {
    opencl_product_renderer_->ReleaseSessionCaches();
  }
#endif
}

};  // namespace alcedo
