//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/pipeline/pipeline_executor.hpp"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "edit/runtime/mask_coverage_display.hpp"
#include "edit/runtime/mask_coverage_presentation.hpp"
#include "image/image_buffer.hpp"
#ifdef HAVE_CUDA
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#endif
#ifdef HAVE_METAL
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"
#endif
#ifdef HAVE_OPENCL
#include "edit/runtime/opencl/opencl_mask_pass.hpp"
#include "edit/runtime/opencl/opencl_renderer.hpp"
#endif

namespace alcedo {

namespace {

#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
/** @brief Renderer of the request role; created on first use. */
template <class RendererType, class Renderers>
auto RoleRenderer(Renderers& renderers, const PipelineApplyRequest& request,
                  const std::shared_ptr<const LutResourceResolver>& lut_resources,
                  const std::shared_ptr<const MaskAssetReader>&     mask_assets) -> RendererType& {
  auto& renderer =
      request.role == ExecutorRole::Interactive ? renderers.interactive : renderers.batch;
  if (!renderer) {
    renderer = std::make_shared<RendererType>(request.role, PreparedSourceCache::UnpackFn{},
                                              lut_resources, mask_assets);
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

/** @brief Coverage-only presentation on the device of the interactive @p renderer. */
template <class RendererType>
auto PresentInteractiveMaskCoverage(const std::shared_ptr<RendererType>& renderer,
                                    const MaskCoverageSelection&         selection,
                                    MaskCoverageInvertOpacity            mask,
                                    const MaskCoverageFrameIdentity&     identity,
                                    IMaskCoverageSink& sink) -> MaskCoveragePublication {
  if (!renderer || !renderer->PresentedFrameGeometry().has_value()) {
    throw std::runtime_error(
        "Mask coverage display: the editor has not presented a photo frame of the image");
  }
  const MaskCoverageDisplayRequest request{.geometry = *renderer->PresentedFrameGeometry(),
                                           .invert   = mask.invert,
                                           .opacity  = mask.opacity};
  return alcedo::PresentMaskCoverage(renderer->Device(), selection, request, identity, sink);
}

template <class Renderers>
auto RoleRendererDeviceBytes(const Renderers& renderers) -> std::size_t {
  std::size_t bytes = 0;
  for (const auto* renderer : {renderers.interactive.get(), renderers.batch.get()}) {
    if (renderer == nullptr) {
      continue;
    }
    const auto resources = renderer->Resources();
    bytes += resources.texture_pool_used_bytes + resources.transient_capacity_bytes +
             resources.parameter_capacity_bytes;
  }
  return bytes;
}
#endif

}  // namespace

PipelineExecutor::PipelineExecutor(ExecutorRole                               role,
                                   std::shared_ptr<const LutResourceResolver> lut_resources,
                                   std::shared_ptr<const MaskAssetReader>     mask_assets)
    : serves_interactive_(role == ExecutorRole::Interactive),
      serves_batch_(role == ExecutorRole::Batch),
      resolved_accelerator_backend_(alcedo::ResolveAcceleratorBackend(accelerator_preference_)),
      lut_resources_(lut_resources ? std::move(lut_resources) : DefaultLutResourceResolver()),
      mask_assets_(mask_assets ? std::move(mask_assets) : DefaultMaskAssetReader()) {}

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
    return RoleRenderer<CudaRenderer>(cuda_renderers_, request, lut_resources_, mask_assets_)
        .RenderImage(snapshot, input, request);
  }
#endif
#ifdef HAVE_METAL
  if (resolved_accelerator_backend_ == GpuBackendKind::Metal) {
    return RoleRenderer<MetalRenderer>(metal_renderers_, request, lut_resources_, mask_assets_)
        .RenderImage(snapshot, input, request);
  }
#endif
#ifdef HAVE_OPENCL
  if (resolved_accelerator_backend_ == GpuBackendKind::OpenCL) {
    return RoleRenderer<OpenClRenderer>(opencl_renderers_, request, lut_resources_, mask_assets_)
        .RenderImage(snapshot, input, request);
  }
#endif
  (void)snapshot;
  (void)input;
  throw std::runtime_error("PipelineExecutor: product rendering requires a supported GPU backend");
}

auto PipelineExecutor::PresentMaskCoverage(const MaskCoverageSelection&     selection,
                                           MaskCoverageInvertOpacity        mask,
                                           const MaskCoverageFrameIdentity& identity,
                                           IMaskCoverageSink& sink) -> MaskCoveragePublication {
  if (!serves_interactive_) {
    throw std::invalid_argument(
        "PipelineExecutor: coverage presentation needs the Interactive role");
  }
#ifdef HAVE_CUDA
  if (resolved_accelerator_backend_ == GpuBackendKind::CUDA) {
    return PresentInteractiveMaskCoverage(cuda_renderers_.interactive, selection, mask, identity,
                                          sink);
  }
#endif
#ifdef HAVE_METAL
  if (resolved_accelerator_backend_ == GpuBackendKind::Metal) {
    return PresentInteractiveMaskCoverage(metal_renderers_.interactive, selection, mask, identity,
                                          sink);
  }
#endif
#ifdef HAVE_OPENCL
  if (resolved_accelerator_backend_ == GpuBackendKind::OpenCL) {
    return PresentInteractiveMaskCoverage(opencl_renderers_.interactive, selection, mask, identity,
                                          sink);
  }
#endif
  (void)selection;
  (void)mask;
  (void)identity;
  (void)sink;
  throw std::runtime_error(
      "PipelineExecutor: coverage presentation requires a supported GPU backend");
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

auto PipelineExecutor::DeviceResourceBytes() const -> std::size_t {
  std::size_t bytes = 0;
#ifdef HAVE_CUDA
  bytes += RoleRendererDeviceBytes(cuda_renderers_);
#endif
#ifdef HAVE_METAL
  bytes += RoleRendererDeviceBytes(metal_renderers_);
#endif
#ifdef HAVE_OPENCL
  bytes += RoleRendererDeviceBytes(opencl_renderers_);
#endif
  return bytes;
}

};  // namespace alcedo
