//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstdio>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>
#include <utility>

#include "cuda/cuda_check.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

void PresentCudaTexture(CudaRenderDevice& device, const GraphValueId& output_id, IFrameSink& sink,
                        const FrameCompletionSubmission& submission) {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("CudaProductRenderer: DRT output is missing");
  }
  auto&      texture   = lease->Texture();
  const auto width     = texture.Width();
  const auto height    = texture.Height();
  const auto row_bytes = static_cast<std::size_t>(width) * sizeof(float4);

  sink.BindFrameSubmission(submission);
  sink.EnsureSize(static_cast<int>(width), static_cast<int>(height));
  const FrameWriteMapping mapping = sink.MapResourceForWrite(FrameMemoryDomain::CudaDevice);
  if (!mapping) {
    return;
  }

  try {
    if (mapping.pixel_format != FramePixelFormat::RGBA32F) {
      throw std::runtime_error("CudaProductRenderer: frame sink requires an RGBA32F target");
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
      throw std::runtime_error("CudaProductRenderer: frame sink target is not CUDA-compatible");
    }
    cuda::CheckCuda(copy_result, "CudaProductRenderer: copy DRT output to frame sink");

    if (mapping.cuda_signal_semaphore != nullptr && mapping.cuda_signal_value != 0) {
      cudaExternalSemaphoreSignalParams signal_params{};
      signal_params.params.fence.value = mapping.cuda_signal_value;
      auto semaphore = reinterpret_cast<cudaExternalSemaphore_t>(mapping.cuda_signal_semaphore);
      cuda::CheckCuda(::cudaSignalExternalSemaphoresAsync(&semaphore, &signal_params, 1,
                                                          device.CommandContext().Stream()),
                      "CudaProductRenderer: signal frame sink");
    }
    cuda::CheckCuda(::cudaStreamSynchronize(device.CommandContext().Stream()),
                    "CudaProductRenderer: wait for frame sink copy");
  } catch (...) {
    sink.UnmapResource();
    throw;
  }
  sink.UnmapResource();
  sink.NotifyFrameReady(submission);
}

auto DownloadCudaTexture(CudaRenderDevice& device, const GraphValueId& output_id)
    -> std::shared_ptr<ImageBuffer> {
  auto* lease = device.Workspace().Images().Find(output_id);
  if (lease == nullptr || lease->Empty()) {
    throw std::runtime_error("CudaProductRenderer: cannot download a missing DRT output");
  }
  auto&   texture = lease->Texture();
  cv::Mat pixels(static_cast<int>(texture.Height()), static_cast<int>(texture.Width()), CV_32FC4);
  auto    bytes = std::span<std::byte>{reinterpret_cast<std::byte*>(pixels.data),
                                       pixels.total() * pixels.elemSize()};
  device.Workspace().Device().DownloadTexture2D(texture, bytes, device.CommandContext());
  return std::make_shared<ImageBuffer>(std::move(pixels));
}

}  // namespace

CudaProductRenderer::CudaProductRenderer(std::shared_ptr<PipelineDocument> document)
    : document_(std::move(document)), device_(std::make_unique<CudaRenderDevice>()) {
  device_->SetErrorReporter([](std::string_view message) {
    std::fprintf(stderr, "[ERROR] CUDA DAG product render failed: %.*s\n",
                 static_cast<int>(message.size()), message.data());
  });
}

CudaProductRenderer::~CudaProductRenderer() = default;

void CudaProductRenderer::SetDocument(std::shared_ptr<PipelineDocument> document) {
  if (!document) {
    throw std::invalid_argument("CudaProductRenderer: PipelineDocument is null");
  }
  document_ = std::move(document);
}

auto CudaProductRenderer::Render(const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                                 const RenderRequest& request, IFrameSink* sink,
                                 const FrameCompletionSubmission& submission,
                                 bool require_host_output) -> std::shared_ptr<ImageBuffer> {
  if (!document_) {
    throw std::runtime_error("CudaProductRenderer: PipelineDocument is not configured");
  }
  if (!input || !input->buffer_valid_) {
    throw std::runtime_error("CudaProductRenderer: product path requires encoded image bytes");
  }

  auto&      encoded       = input->GetBuffer();
  const auto encoded_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  auto       prepared  = RawInputLoader::LoadEncoded(encoded_bytes, decode_res);
  const auto plan      = GraphCompiler::Compile(*document_, prepared.CompileSource(), request);
  const auto output_id = device_->Execute(plan, prepared, *document_);

  try {
    if (sink != nullptr) {
      PresentCudaTexture(*device_, output_id, *sink, submission);
    }
    if (require_host_output) {
      return DownloadCudaTexture(*device_, output_id);
    }
  } catch (const std::exception& ex) {
    device_->ReportError(ex.what());
    throw;
  }
  return std::make_shared<ImageBuffer>();
}

}  // namespace alcedo
