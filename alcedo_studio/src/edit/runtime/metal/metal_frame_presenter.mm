//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_frame_presenter.hpp"

#include <span>
#include <stdexcept>

#include <alcedo/metal/Metal.hpp>
#include <opencv2/core.hpp>

#include "edit/scope/detail/scope_metal_shared.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

void FramePresenter<MetalBackend>::Present(BasicRenderDevice<MetalBackend>& device,
                                           const GraphValueId& output_id, IFrameSink& sink,
                                           const FrameCompletionSubmission& submission,
                                           const ViewerDisplayConfig&       display_config) {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("MetalRenderer: DRT output is missing");
  }
  auto* native = static_cast<MTL::Texture*>(lease->Texture().Native());
  if (native == nullptr) {
    throw std::runtime_error("MetalRenderer: DRT output texture is null");
  }
  auto* command_buffer =
      static_cast<MTL::CommandBuffer*>(device.CommandContext().NativeCommandBuffer());
  if (command_buffer == nullptr) {
    throw std::runtime_error("MetalRenderer: present requires the frame command buffer");
  }

  const auto width  = static_cast<int>(lease->Texture().Width());
  const auto height = static_cast<int>(lease->Texture().Height());

  auto image            = std::make_shared<scope::metal_detail::MetalTextureImageResource>();
  image->texture        = NS::RetainPtr(native);
  image->width          = width;
  image->height         = height;
  image->format         = FramePixelFormat::RGBA32F;
  image->native_object  = reinterpret_cast<std::uintptr_t>(image->texture.get());

  auto ready            = std::make_shared<scope::metal_detail::MetalCommandBufferSignalResource>();
  ready->command_buffer = NS::RetainPtr(command_buffer);

  sink.BindFrameSubmission(submission);
  sink.EnsureSize(width, height);
  sink.SubmitFinalDisplayFrame(FinalDisplayFrameView{
      SharedGpuImageHandle{GpuBackend::Metal, std::shared_ptr<void>(image, image.get()), width,
                           height, 0, FramePixelFormat::RGBA32F},
      width, height, FramePixelFormat::RGBA32F, display_config, AnalysisDomain::DisplayEncoded,
      GpuSignalHandle{GpuBackend::Metal, std::shared_ptr<void>(ready, ready.get())}, 0});
  sink.SubmitMetalFrame(ViewerMetalFrame{
      width, height, image->native_object,
      std::shared_ptr<const void>(image, image->texture.get()), display_config, submission.mode,
      submission.metadata});
  sink.NotifyFrameReady(submission);
}

auto FramePresenter<MetalBackend>::Download(BasicRenderDevice<MetalBackend>& device,
                                            const GraphValueId&              output_id)
    -> std::shared_ptr<ImageBuffer> {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("MetalRenderer: cannot download a missing DRT output");
  }
  auto&   texture = lease->Texture();
  cv::Mat pixels(static_cast<int>(texture.Height()), static_cast<int>(texture.Width()), CV_32FC4);
  auto    bytes = std::span<std::byte>{reinterpret_cast<std::byte*>(pixels.data),
                                       pixels.total() * pixels.elemSize()};
  device.Workspace().Device().DownloadTexture2D(texture, bytes, device.CommandContext());
  return std::make_shared<ImageBuffer>(std::move(pixels));
}

}  // namespace alcedo
