//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/renderer.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace detail {

template <class Backend>
void TraceGpuDagGeometry(const ExecutionPlan& plan, const RenderRequest& request,
                         const FrameCompletionSubmission& submission) {
  const char* enabled = std::getenv("ALCEDO_ROI_TRACE");
  if (enabled == nullptr || enabled[0] == '\0' || enabled[0] == '0') {
    return;
  }
  const auto& view                  = request.view.visible_rect_in_edit_space;
  const auto& geometry              = plan.geometry;
  const auto  native_visible_width  = static_cast<float>(geometry.edit_extent.width) * view.w;
  const auto  native_visible_height = static_cast<float>(geometry.edit_extent.height) * view.h;
  const bool  nearest_viewer_expansion =
      !view.IsFullFrame() && (request.view.viewport_extent.width > geometry.render_extent.width ||
                              request.view.viewport_extent.height > geometry.render_extent.height);
  std::fprintf(
      stderr,
      "[ROI_TRACE][gpu-dag-geometry] backend=%s request=%llu role=%d mode=%d "
      "decoded=%ux%u full_ref=%ux%u edit=%ux%u roi=%.6f,%.6f,%.6f,%.6f "
      "native_roi=%.2fx%.2f viewport_target=%ux%u max_edge=%u render=%ux%u filter=%d "
      "viewer_nearest_expand=%d required_decoded=%d,%d,%d,%d\n",
      Backend::kName, static_cast<unsigned long long>(submission.metadata.presentation_request_id),
      static_cast<int>(submission.metadata.frame_role), static_cast<int>(submission.mode),
      geometry.decoded_extent.width, geometry.decoded_extent.height,
      geometry.full_reference_extent.width, geometry.full_reference_extent.height,
      geometry.edit_extent.width, geometry.edit_extent.height, view.x, view.y, view.w, view.h,
      native_visible_width, native_visible_height, request.view.viewport_extent.width,
      request.view.viewport_extent.height, request.resolution.max_edge,
      geometry.render_extent.width, geometry.render_extent.height,
      static_cast<int>(geometry.filter), nearest_viewer_expansion ? 1 : 0,
      geometry.required_decoded_region.x, geometry.required_decoded_region.y,
      geometry.required_decoded_region.width, geometry.required_decoded_region.height);
}

}  // namespace detail

template <class Backend>
auto Renderer<Backend>::Render(const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                               const RenderRequest& request, IFrameSink* sink,
                               const FrameCompletionSubmission& submission,
                               bool require_host_output, RenderCachePolicy cache_policy)
    -> std::shared_ptr<ImageBuffer> {
  if (!document_) {
    throw std::runtime_error("Renderer: PipelineDocument is not configured");
  }
  if (!input || !input->buffer_valid_) {
    throw std::runtime_error("Renderer: product path requires encoded image bytes");
  }

  auto&      encoded       = input->GetBuffer();
  const auto encoded_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  const bool use_session_cache = cache_policy == RenderCachePolicy::UseSessionCache;
  if (!use_session_cache) {
    EnsureOneShotDevice();
  }

  std::optional<PreparedSourceCache::Lease> prepared_lease;
  std::optional<PreparedRawInput>           one_shot_prepared;
  ExecutionPlan                             plan;
  RenderDevice*                             render_device = device_.get();
  if (use_session_cache) {
    prepared_lease.emplace(source_cache_.AcquireEncoded(encoded_bytes, decode_res));
    plan = plan_cache_.GetOrCompile(*document_, prepared_lease->Get().CompileSource());
  } else {
    one_shot_prepared.emplace(unpack_(encoded_bytes, decode_res));
    plan          = GraphCompiler::CompileStatic(*document_, one_shot_prepared->CompileSource(),
                                                 Backend::kCapabilityVersion);
    render_device = one_shot_device_.get();
  }
  GraphCompiler::BindFrameGeometry(plan, *document_, request);
  detail::TraceGpuDagGeometry<Backend>(plan, request, submission);
  if (document_->TopologyDirty()) {
    document_->ClearTopologyDirty();
  }
  const auto& prepared = use_session_cache ? prepared_lease->Get() : *one_shot_prepared;
  const auto  output_id =
      render_device->Execute(plan, prepared, *document_, mask_store_.get(), false);
  const auto release_one_shot_resources = [&]() {
    if (use_session_cache) {
      return;
    }
    render_device->Workspace().Images().DiscardUnpublished();
    render_device->WaitIdle();
    render_device->Workspace().ReleaseSessionResources();
    render_device->ReleaseNeuralDemosaicWorkspace();
  };

  ViewerDisplayConfig display_config{};
  if (const auto* drt = document_->Drt()) {
    display_config = ViewerDisplayConfigFromDrt(drt->Params().Params());
  }

  try {
    if (sink != nullptr) {
      FramePresenter<Backend>::Present(*render_device, output_id, *sink, submission,
                                       display_config);
    }
    if (require_host_output) {
      auto host = FramePresenter<Backend>::Download(*render_device, output_id);
      if (use_session_cache) {
        render_device->PublishResults();
      } else {
        release_one_shot_resources();
      }
      return host;
    }
    if (use_session_cache) {
      render_device->PublishResults();
    } else {
      release_one_shot_resources();
    }
  } catch (const std::exception& ex) {
    try {
      if (render_device->Workspace().IsRendering()) {
        render_device->CancelRender();
      } else {
        render_device->WaitIdle();
      }
      render_device->Workspace().Images().DiscardUnpublished();
      if (!use_session_cache) {
        render_device->WaitIdle();
        render_device->Workspace().ReleaseSessionResources();
        render_device->ReleaseNeuralDemosaicWorkspace();
      }
    } catch (...) {
      // Preserve the original presentation/download error. The device destructor or
      // session teardown still owns the last-resort wait and resource release.
    }
    render_device->ReportError(ex.what());
    throw;
  }
  return std::make_shared<ImageBuffer>();
}

}  // namespace alcedo
