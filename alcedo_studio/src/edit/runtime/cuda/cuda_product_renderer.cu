//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>

#include "cuda/cuda_check.hpp"
#include "edit/runtime/cuda/cuda_frame_presenter.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/scope/detail/scope_cuda_shared.cuh"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

void SubmitCudaDisplayFrame(IFrameSink& sink, const CudaBackend::Texture2D& texture,
                            cudaStream_t stream, const FrameWriteMapping& mapping,
                            const ViewerDisplayConfig& display_config) {
  const auto width     = static_cast<int>(texture.Width());
  const auto height    = static_cast<int>(texture.Height());
  const auto row_bytes = static_cast<std::size_t>(width) * sizeof(float4);

  auto final_image           = std::make_shared<scope::cuda_detail::CudaLinearImageResource>();
  final_image->device_ptr    = texture.DevicePointer();
  final_image->row_bytes     = row_bytes;
  final_image->width         = width;
  final_image->height        = height;
  final_image->format        = FramePixelFormat::RGBA32F;
  final_image->owns_memory   = false;
  final_image->native_object = mapping.native_object;

  auto ready_signal    = std::make_shared<scope::cuda_detail::CudaStreamSignalResource>();
  ready_signal->stream = stream;

  sink.SubmitFinalDisplayFrame(FinalDisplayFrameView{
      SharedGpuImageHandle{GpuBackend::Cuda, std::move(final_image), width, height, row_bytes,
                           FramePixelFormat::RGBA32F},
      width, height, FramePixelFormat::RGBA32F, display_config, AnalysisDomain::DisplayEncoded,
      GpuSignalHandle{GpuBackend::Cuda, std::move(ready_signal)}, 0});
}

}  // namespace

void FramePresenter<CudaBackend>::Present(CudaRenderDevice& device, const GraphValueId& output_id,
                                          IFrameSink& sink,
                                          const FrameCompletionSubmission& submission,
                                          const ViewerDisplayConfig&       display_config) {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("CudaRenderer: DRT output is missing");
  }
  auto&      texture   = lease->Texture();
  const auto width     = texture.Width();
  const auto height    = texture.Height();
  const auto row_bytes = static_cast<std::size_t>(width) * sizeof(float4);

  sink.BindFrameSubmission(submission);
  sink.EnsureSize(static_cast<int>(width), static_cast<int>(height));
  const FrameWriteMapping mapping = sink.MapResourceForWrite(FrameMemoryDomain::CudaDevice);
  if (!mapping) {
    throw std::runtime_error("CudaRenderer: frame sink rejected the write mapping");
  }

  try {
    if (mapping.pixel_format != FramePixelFormat::RGBA32F) {
      throw std::runtime_error("CudaRenderer: frame sink requires an RGBA32F target");
    }
    cudaError_t copy_result = cudaSuccess;
    if (mapping.target_type == FrameWriteTargetType::LinearBuffer) {
      const auto copy_kind = mapping.memory_domain == FrameMemoryDomain::CudaDevice
                                 ? cudaMemcpyDeviceToDevice
                                 : cudaMemcpyDeviceToHost;
      copy_result =
          ::cudaMemcpy2DAsync(mapping.data, mapping.row_bytes, texture.DevicePointer(), row_bytes,
                              row_bytes, height, copy_kind, device.CommandContext().Stream());
    } else if (mapping.target_type == FrameWriteTargetType::CudaArray &&
               mapping.memory_domain == FrameMemoryDomain::CudaDevice) {
      copy_result = ::cudaMemcpy2DToArrayAsync(
          reinterpret_cast<cudaArray_t>(mapping.image_array), 0, 0, texture.DevicePointer(),
          row_bytes, row_bytes, height, cudaMemcpyDeviceToDevice, device.CommandContext().Stream());
    } else {
      throw std::runtime_error("CudaRenderer: frame sink target is not CUDA-compatible");
    }
    cuda::CheckCuda(copy_result, "CudaRenderer: copy DRT output to frame sink");

    if (mapping.cuda_signal_semaphore != nullptr && mapping.cuda_signal_value != 0) {
      cudaExternalSemaphoreSignalParams signal_params{};
      signal_params.params.fence.value = mapping.cuda_signal_value;
      auto semaphore = reinterpret_cast<cudaExternalSemaphore_t>(mapping.cuda_signal_semaphore);
      cuda::CheckCuda(::cudaSignalExternalSemaphoresAsync(&semaphore, &signal_params, 1,
                                                          device.CommandContext().Stream()),
                      "CudaRenderer: signal frame sink");
    }
    SubmitCudaDisplayFrame(sink, texture, device.CommandContext().Stream(), mapping,
                           display_config);
    cuda::CheckCuda(::cudaStreamSynchronize(device.CommandContext().Stream()),
                    "CudaRenderer: wait for frame sink copy");
  } catch (...) {
    sink.UnmapResource();
    throw;
  }
  sink.UnmapResource();
  sink.NotifyFrameReady(submission);
}

auto FramePresenter<CudaBackend>::Download(CudaRenderDevice& device, const GraphValueId& output_id)
    -> std::shared_ptr<ImageBuffer> {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("CudaRenderer: cannot download a missing DRT output");
  }
  auto&   texture = lease->Texture();
  cv::Mat pixels(static_cast<int>(texture.Height()), static_cast<int>(texture.Width()), CV_32FC4);
  auto    bytes = std::span<std::byte>{reinterpret_cast<std::byte*>(pixels.data),
                                       pixels.total() * pixels.elemSize()};
  device.Workspace().Device().DownloadTexture2D(texture, bytes, device.CommandContext());
  return std::make_shared<ImageBuffer>(std::move(pixels));
}

}  // namespace alcedo
