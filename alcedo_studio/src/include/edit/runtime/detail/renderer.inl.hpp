//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/runtime/develop_demosaic.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/renderer.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "image/image_buffer.hpp"
#include "utils/diagnostics/preview_performance.hpp"

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
auto Renderer<Backend>::Render(const PipelineGraphSnapshot&        snapshot,
                               const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                               const RenderRequest& request, IFrameSink* sink,
                               const FrameCompletionSubmission& submission,
                               bool require_host_output,
                               const std::optional<ExportColorProfileConfig>& output_color)
    -> std::shared_ptr<ImageBuffer> {
  PipelineApplyRequest apply;
  apply.geometry            = request;
  apply.decode_res          = decode_res;
  apply.role                = role_;
  apply.require_host_output = require_host_output;
  apply.sink                = sink;
  apply.submission          = submission;
  apply.output_color        = output_color;
  return Render(snapshot, input, apply);
}

template <class Backend>
auto Renderer<Backend>::Render(const PipelineGraphSnapshot&        snapshot,
                               const std::shared_ptr<ImageBuffer>& input,
                               const PipelineApplyRequest&         request)
    -> std::shared_ptr<ImageBuffer> {
  if (request.role != role_) {
    throw std::invalid_argument("Renderer: request role does not match the renderer role");
  }
  const bool interactive = role_ == ExecutorRole::Interactive;
  if (request.prepared_input) {
    if (interactive) {
      throw std::invalid_argument("Renderer: an interactive render takes encoded bytes only");
    }
    if (request.prepared_input->downsample_passes !=
        DecodeResToDownsamplePasses(request.decode_res)) {
      throw std::invalid_argument("Renderer: prepared input does not match the decode resolution");
    }
  } else if (!input || !input->buffer_valid_) {
    throw std::runtime_error("Renderer: product path requires encoded image bytes");
  }
  if (interactive) {
    const auto key = RenderBindingKey::Of(snapshot);
    if (binding_.has_value() && *binding_ != key) {
      ReleaseBinding();
    }
    binding_ = key;
  }
  const PipelineDocument& document = snapshot.Document();

  // Encoded bytes; empty when the request carries a prepared input.
  std::span<const std::byte> encoded_bytes;
  if (input && input->buffer_valid_) {
    auto& encoded = input->GetBuffer();
    encoded_bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(encoded.data()),
                                               encoded.size()};
  }
  EnsureDevice();

  std::optional<PreparedSourceCache::Lease> prepared_lease;
  std::shared_ptr<const PreparedRawInput>   batch_prepared;
  ExecutionPlan                             plan;
  RenderDevice* const                       render_device = device_.get();
  struct BoundPreviewRequest {
    explicit BoundPreviewRequest(std::uint64_t request_id) {
      diag::PreviewPerformance::BindCurrentRequest(request_id);
    }
    ~BoundPreviewRequest() { diag::PreviewPerformance::ClearCurrentRequest(); }
  };
  BoundPreviewRequest bound_request(request.submission.metadata.presentation_request_id);
  if (interactive) {
    prepared_lease.emplace(source_cache_.AcquireEncoded(encoded_bytes, request.decode_res));
    plan = plan_cache_.GetOrCompile(document, prepared_lease->Get().CompileSource());
  } else {
    batch_prepared = request.prepared_input
                         ? request.prepared_input
                         : std::make_shared<const PreparedRawInput>(
                               unpack_(encoded_bytes, request.decode_res));
    diag::PreviewCpuInterval compile(diag::PreviewCpuStage::PlanCompile);
    plan = GraphCompiler::CompileStatic(document, batch_prepared->CompileSource(),
                                        Backend::kCapabilityVersion);
  }
  GraphCompiler::BindFrameGeometry(plan, document, request.geometry);
  plan.output_color_override = request.output_color;
  if (diag::PreviewPerformanceEnabled()) {
    diag::PreviewPerformance::NoteRenderExtent(plan.geometry.render_extent.width,
                                               plan.geometry.render_extent.height);
  }
  detail::TraceGpuDagGeometry<Backend>(plan, request.geometry, request.submission);
  const auto& prepared = interactive ? prepared_lease->Get() : *batch_prepared;
  const auto  persistence =
      interactive ? ResultPersistenceScopeForRole(request.submission.metadata.frame_role)
                  : ResultPersistenceScope::AllCurrentResults;
  if (diag::PreviewPerformanceEnabled()) {
    diag::PreviewDevelopDecodeParams develop;
    switch (request.decode_res) {
      case DecodeRes::HALF:
        develop.decode_res = diag::PreviewDecodeRes::Half;
        break;
      case DecodeRes::QUARTER:
        develop.decode_res = diag::PreviewDecodeRes::Quarter;
        break;
      case DecodeRes::EIGHTH:
        develop.decode_res = diag::PreviewDecodeRes::Eighth;
        break;
      case DecodeRes::FULL:
      default:
        develop.decode_res = diag::PreviewDecodeRes::Full;
        break;
    }
    switch (prepared.CompileSource().kind) {
      case DevelopInputKind::XTransCfa:
        develop.cfa = diag::PreviewCfaKind::XTrans;
        break;
      case DevelopInputKind::DirectRgb:
        develop.cfa        = diag::PreviewCfaKind::DirectRgb;
        develop.upload_rgb = true;
        develop.layout     = diag::PreviewDevelopLayout::UploadRgb;
        break;
      case DevelopInputKind::BayerCfa:
      default:
        develop.cfa = diag::PreviewCfaKind::Bayer;
        break;
    }
    const auto* develop_node = document.Develop();
    const auto  method       = develop_node == nullptr
                                   ? RawDemosaicMethod::Legacy
                                   : ResolveDevelopDemosaicMethod(develop_node->Params().Params(),
                                                                  prepared.CompileSource());
    develop.demosaic         = method == RawDemosaicMethod::NeuralEngine
                                   ? diag::PreviewDemosaicMethod::NeuralEngine
                                   : diag::PreviewDemosaicMethod::Legacy;
    develop.highlights_reconstruct =
        develop_node != nullptr && develop_node->Params().Params().highlights_reconstruct;
    develop.downsample_passes = prepared.downsample_passes;
    develop.host_width        = prepared.host_extent.width;
    develop.host_height       = prepared.host_extent.height;
    develop.develop_width     = plan.source.develop_output_extent.width;
    develop.develop_height    = plan.source.develop_output_extent.height;
    develop.full_ref_width    = plan.source.full_reference_extent.width;
    develop.full_ref_height   = plan.source.full_reference_extent.height;
    diag::PreviewPerformance::NoteDevelopDecode(develop);
  }
  GraphValueId output_id;
  {
    diag::PreviewCpuInterval encode(diag::PreviewCpuStage::Encode);
    output_id = render_device->Execute(plan, prepared, document, false,
                                       interactive ? TransientAllocationPolicy::SessionPacked
                                                   : TransientAllocationPolicy::ExactRelease,
                                       persistence);
  }
  if (diag::PreviewPerformanceEnabled()) {
    diag::PreviewPerformance::NoteResourceSnapshot(
        render_device->Workspace().CaptureResourceSnapshot());
  }
  const auto release_batch_resources = [&]() {
    if (interactive) {
      return;
    }
    render_device->Workspace().Images().DiscardUnpublished();
    render_device->WaitIdle();
    render_device->Workspace().ReleaseSessionResources();
    render_device->ReleaseNeuralDemosaicWorkspace();
  };

  ViewerDisplayConfig display_config{};
  if (request.output_color.has_value()) {
    display_config.encoding_space = request.output_color->encoding_space;
    display_config.encoding_eotf  = request.output_color->encoding_eotf;
    display_config.peak_luminance = request.output_color->peak_luminance;
  } else if (const auto* drt = document.Drt()) {
    display_config = ViewerDisplayConfigFromDrt(drt->Params().Params());
  }

  const auto finish_successful_session = [&]() {
    if (!interactive) {
      release_batch_resources();
      return;
    }
    render_device->PublishResults();
    render_device->Workspace().ResultInvalidation().CompleteMatchingImages(
        render_device->Workspace().Images());
    if (persistence != ResultPersistenceScope::SensorDevelopOnly) {
      return;
    }
    render_device->WaitIdle();
    if (request.sink == nullptr) {
      render_device->Workspace().Images().DiscardUnpublished();
    } else {
      render_device->Workspace().Images().ReleaseUnpublishedExcept(output_id);
    }
  };

  try {
    if (request.sink != nullptr) {
      // Attach the exact resolved geometry so viewer-side Mask mapping uses the
      // frame being presented instead of a separately derived document mapping.
      FrameCompletionSubmission presented = request.submission;
      presented.geometry                  = plan.geometry;
      FramePresenter<Backend>::Present(*render_device, output_id, *request.sink, presented,
                                       display_config);
    }
    if (request.require_host_output) {
      auto host = FramePresenter<Backend>::Download(*render_device, output_id);
      finish_successful_session();
      return host;
    }
    finish_successful_session();
  } catch (const std::exception& ex) {
    try {
      if (render_device->Workspace().IsRendering()) {
        render_device->CancelRender();
      } else {
        render_device->WaitIdle();
      }
      render_device->Workspace().Images().DiscardUnpublished();
      if (!interactive) {
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
