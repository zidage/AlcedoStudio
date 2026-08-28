//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_frame_presenter.hpp"

#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>

#include "edit/scope/detail/scope_opencl_shared.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "opencl/opencl_check.hpp"

namespace alcedo {

void FramePresenter<OpenClBackend>::Present(BasicRenderDevice<OpenClBackend>& device,
                                            const GraphValueId& output_id, IFrameSink& sink,
                                            const FrameCompletionSubmission& submission,
                                            const ViewerDisplayConfig&       display_config) {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("OpenClRenderer: DRT output is missing");
  }
  auto& texture = lease->Texture();
  if (texture.Native() == nullptr || texture.Format() != TextureFormat::Rgba32f) {
    throw std::runtime_error("OpenClRenderer: DRT output is not an RGBA32F image");
  }

  const int width  = static_cast<int>(texture.Width());
  const int height = static_cast<int>(texture.Height());
  sink.BindFrameSubmission(submission);
  sink.EnsureSize(width, height);

  bool resource_mapped = false;
  try {
    const auto mapping = sink.MapResourceForWrite(FrameMemoryDomain::OpenClDevice);
    resource_mapped    = true;
    if (!mapping || mapping.pixel_format != FramePixelFormat::RGBA32F ||
        mapping.memory_domain != FrameMemoryDomain::OpenClDevice ||
        mapping.target_type != FrameWriteTargetType::OpenClImage || mapping.data == nullptr) {
      throw std::runtime_error(
          "OpenClRenderer: sink did not provide an OpenCL image2d RGBA32F target");
    }

    auto*             destination = reinterpret_cast<cl_mem>(mapping.data);
    const std::size_t origin[3]   = {0, 0, 0};
    const std::size_t region[3]   = {texture.Width(), texture.Height(), 1};
    cl_event          copy_event  = nullptr;
    CheckOpenCl(clEnqueueCopyImage(device.Workspace().Device().NativeQueue(), texture.Native(),
                                   destination, origin, origin, region, 0, nullptr, &copy_event),
                "OpenClRenderer: clEnqueueCopyImage(present)");
    device.Workspace().Device().TrackKernelEvent(device.CommandContext(), copy_event);

    sink.UnmapResource();
    resource_mapped = false;
    device.Workspace().Device().FinalizePresentation(device.CommandContext());

    const auto final_event = device.CommandContext().FinalEvent();
    if (final_event == nullptr) {
      throw std::runtime_error("OpenClRenderer: presentation completion event is missing");
    }
    CheckOpenCl(clRetainEvent(final_event), "OpenClRenderer: clRetainEvent(present)");

    auto image           = std::make_shared<scope::opencl_detail::OpenClImageResource>();
    image->image         = texture.Native();
    image->width         = width;
    image->height        = height;
    image->format        = FramePixelFormat::RGBA32F;
    image->native_object = reinterpret_cast<std::uintptr_t>(image->image);
    CheckOpenCl(clRetainMemObject(image->image), "OpenClRenderer: clRetainMemObject(scope image)");
    image->owns_memory = true;

    auto ready         = std::make_shared<scope::opencl_detail::OpenClEventSignalResource>();
    ready->event       = final_event;
    SharedGpuImageHandle image_handle;
    image_handle.backend       = GpuBackend::OpenCL;
    image_handle.resource      = std::shared_ptr<void>(image, image.get());
    image_handle.width         = width;
    image_handle.height        = height;
    image_handle.format        = FramePixelFormat::RGBA32F;
    image_handle.resource_type = FrameWriteTargetType::OpenClImage;

    sink.SubmitFinalDisplayFrame(FinalDisplayFrameView{
        image_handle, width, height, FramePixelFormat::RGBA32F, display_config,
        AnalysisDomain::DisplayEncoded,
        GpuSignalHandle{GpuBackend::OpenCL, std::shared_ptr<void>(ready, ready.get())}, 0, 0, 0,
        submission.metadata.presentation_request_id});
    sink.NotifyFrameReady(submission);
  } catch (...) {
    if (resource_mapped) {
      try {
        sink.UnmapResource();
      } catch (...) {
      }
    }
    throw;
  }
}

auto FramePresenter<OpenClBackend>::Download(BasicRenderDevice<OpenClBackend>& device,
                                             const GraphValueId&               output_id)
    -> std::shared_ptr<ImageBuffer> {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("OpenClRenderer: cannot download a missing DRT output");
  }
  auto&   texture = lease->Texture();
  cv::Mat pixels(static_cast<int>(texture.Height()), static_cast<int>(texture.Width()), CV_32FC4);
  auto    bytes = std::span<std::byte>{reinterpret_cast<std::byte*>(pixels.data),
                                       pixels.total() * pixels.elemSize()};
  device.Workspace().Device().DownloadTexture2D(texture, bytes, device.CommandContext());
  return std::make_shared<ImageBuffer>(std::move(pixels));
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
