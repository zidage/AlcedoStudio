//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/pipeline/pipeline_executor.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "image/image_buffer.hpp"
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
/** @brief Renderer of the request role; created on first use. */
template <class RendererType, class Renderers>
auto RoleRenderer(Renderers& renderers, const PipelineApplyRequest& request,
                  const std::shared_ptr<const LutResourceResolver>& lut_resources)
    -> RendererType& {
  auto& renderer =
      request.role == ExecutorRole::Interactive ? renderers.interactive : renderers.batch;
  if (!renderer) {
    renderer = std::make_shared<RendererType>(request.role, PreparedSourceCache::UnpackFn{},
                                              lut_resources);
  }
  return *renderer;
}

template <class Renderers>
void ReleaseRoleRenderers(Renderers& renderers) {
  if (renderers.interactive) {
    renderers.interactive->ReleaseBinding();
  }
  if (renderers.batch) {
    renderers.batch->ReleaseBinding();
  }
}
#endif

}  // namespace

PipelineExecutor::PipelineExecutor(ExecutorRole                               role,
                                   std::shared_ptr<const LutResourceResolver> lut_resources)
    : serves_interactive_(role == ExecutorRole::Interactive),
      serves_batch_(role == ExecutorRole::Batch),
      resolved_accelerator_backend_(alcedo::ResolveAcceleratorBackend(accelerator_preference_)),
      lut_resources_(lut_resources ? std::move(lut_resources) : DefaultLutResourceResolver()) {}

auto PipelineExecutor::Apply(const PipelineGraphSnapshot& snapshot,
                             std::shared_ptr<ImageBuffer> input,
                             const PipelineApplyRequest&  request) -> std::shared_ptr<ImageBuffer> {
  return Render(snapshot, input, request).pixels;
}

auto PipelineExecutor::ApplyImage(const PipelineGraphSnapshot& snapshot,
                                  std::shared_ptr<ImageBuffer> input,
                                  const PipelineApplyRequest&  request) -> RenderedPipelineImage {
  if (!request.require_host_output) {
    throw std::invalid_argument("PipelineExecutor: an image render requires host output");
  }
  return Render(snapshot, input, request);
}

auto PipelineExecutor::Render(const PipelineGraphSnapshot&        snapshot,
                              const std::shared_ptr<ImageBuffer>& input,
                              const PipelineApplyRequest& request) -> RenderedPipelineImage {
  if (!Serves(request.role)) {
    throw std::invalid_argument("PipelineExecutor: this executor does not serve the request role");
  }
#ifdef HAVE_CUDA
  if (resolved_accelerator_backend_ == GpuBackendKind::CUDA) {
    return RoleRenderer<CudaRenderer>(cuda_renderers_, request, lut_resources_)
        .RenderImage(snapshot, input, request);
  }
#endif
#ifdef HAVE_METAL
  if (resolved_accelerator_backend_ == GpuBackendKind::Metal) {
    return RoleRenderer<MetalRenderer>(metal_renderers_, request, lut_resources_)
        .RenderImage(snapshot, input, request);
  }
#endif
#ifdef HAVE_OPENCL
  if (resolved_accelerator_backend_ == GpuBackendKind::OpenCL) {
    return RoleRenderer<OpenClRenderer>(opencl_renderers_, request, lut_resources_)
        .RenderImage(snapshot, input, request);
  }
#endif
  (void)snapshot;
  (void)input;
  throw std::runtime_error("PipelineExecutor: product rendering requires a supported GPU backend");
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

void PipelineExecutor::ReleaseBinding() {
#ifdef HAVE_CUDA
  ReleaseRoleRenderers(cuda_renderers_);
#endif
#ifdef HAVE_METAL
  ReleaseRoleRenderers(metal_renderers_);
#endif
#ifdef HAVE_OPENCL
  ReleaseRoleRenderers(opencl_renderers_);
#endif
}

};  // namespace alcedo
