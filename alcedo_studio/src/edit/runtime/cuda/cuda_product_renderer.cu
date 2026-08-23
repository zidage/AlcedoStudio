//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstdio>
#include <filesystem>
#include <opencv2/core.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "cuda/cuda_check.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/scope/detail/scope_cuda_shared.cuh"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

auto ViewerDisplayConfigFromDrt(const DrtPayload& payload) -> ViewerDisplayConfig {
  ViewerDisplayConfig config;
  switch (payload.encoding_space) {
    case DrtColorSpace::Rec2020:
      config.encoding_space = ColorUtils::ColorSpace::REC2020;
      break;
    case DrtColorSpace::P3D65:
      config.encoding_space = ColorUtils::ColorSpace::P3_D65;
      break;
    case DrtColorSpace::Rec709:
    default:
      config.encoding_space = ColorUtils::ColorSpace::REC709;
      break;
  }
  switch (payload.encoding_eotf) {
    case DrtEotf::Linear:
      config.encoding_eotf = ColorUtils::EOTF::LINEAR;
      break;
    case DrtEotf::St2084:
      config.encoding_eotf = ColorUtils::EOTF::ST2084;
      break;
    case DrtEotf::Hlg:
      config.encoding_eotf = ColorUtils::EOTF::HLG;
      break;
    case DrtEotf::Gamma26:
      config.encoding_eotf = ColorUtils::EOTF::GAMMA_2_6;
      break;
    case DrtEotf::Bt1886:
      config.encoding_eotf = ColorUtils::EOTF::BT1886;
      break;
    case DrtEotf::Gamma18:
      config.encoding_eotf = ColorUtils::EOTF::GAMMA_1_8;
      break;
    case DrtEotf::Gamma22:
    default:
      config.encoding_eotf = ColorUtils::EOTF::GAMMA_2_2;
      break;
  }
  config.peak_luminance = payload.peak_luminance;
  return config;
}

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

void PresentCudaTexture(CudaRenderDevice& device, const GraphValueId& output_id, IFrameSink& sink,
                        const FrameCompletionSubmission& submission,
                        const ViewerDisplayConfig& display_config) {
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
    throw std::runtime_error("CudaProductRenderer: frame sink rejected the write mapping");
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
    // Histogram/waveform StageFrame copies this DRT texture on the same stream.
    // Submit before synchronize so that copy is enqueued while the pointer is valid.
    SubmitCudaDisplayFrame(sink, texture, device.CommandContext().Stream(), mapping,
                           display_config);
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
    : CudaProductRenderer(std::move(document), PreparedSourceCache::UnpackFn{}) {}

CudaProductRenderer::CudaProductRenderer(std::shared_ptr<PipelineDocument> document,
                                         PreparedSourceCache::UnpackFn     unpack)
    : document_(std::move(document)),
      device_(std::make_unique<CudaRenderDevice>()),
      one_shot_device_(),
      mask_store_(std::make_unique<MaskStore>(std::filesystem::temp_directory_path() /
                                              "alcedo_studio" / "product_mask_store")),
      unpack_(unpack ? std::move(unpack)
                     : PreparedSourceCache::UnpackFn{[](std::span<const std::byte> encoded,
                                                        DecodeRes                  decode_res) {
                         return RawInputLoader::LoadEncoded(encoded, decode_res);
                       }}),
      source_cache_(unpack_),
      plan_cache_(kCudaDagBackendCapabilityVersion) {
  device_->Workspace().Textures().SetByteBudget(DefaultProductTextureBudgetBytes());
  device_->SetErrorReporter([](std::string_view message) {
    std::fprintf(stderr, "[ERROR] CUDA DAG product render failed: %.*s\n",
                 static_cast<int>(message.size()), message.data());
  });
}

CudaProductRenderer::~CudaProductRenderer() = default;

auto CudaProductRenderer::MaskAssets() -> MaskStore& { return *mask_store_; }

void CudaProductRenderer::SetDocument(std::shared_ptr<PipelineDocument> document) {
  if (!document) {
    throw std::invalid_argument("CudaProductRenderer: PipelineDocument is null");
  }
  document_ = std::move(document);
}

auto CudaProductRenderer::Render(const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                                 const RenderRequest& request, IFrameSink* sink,
                                 const FrameCompletionSubmission& submission,
                                 bool require_host_output, CudaProductCachePolicy cache_policy)
    -> std::shared_ptr<ImageBuffer> {
  if (!document_) {
    throw std::runtime_error("CudaProductRenderer: PipelineDocument is not configured");
  }
  if (!input || !input->buffer_valid_) {
    throw std::runtime_error("CudaProductRenderer: product path requires encoded image bytes");
  }

  auto&      encoded       = input->GetBuffer();
  const auto encoded_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  const bool use_session_cache = cache_policy == CudaProductCachePolicy::UseSessionCache;
  if (!use_session_cache && !one_shot_device_) {
    one_shot_device_ = std::make_unique<CudaRenderDevice>();
    one_shot_device_->Workspace().Textures().SetByteBudget(DefaultProductTextureBudgetBytes());
    one_shot_device_->SetErrorReporter([](std::string_view message) {
      std::fprintf(stderr, "[ERROR] CUDA DAG one-shot render failed: %.*s\n",
                   static_cast<int>(message.size()), message.data());
    });
  }

  std::optional<PreparedSourceCache::Lease> prepared_lease;
  std::optional<PreparedRawInput>           one_shot_prepared;
  ExecutionPlan                             plan;
  CudaRenderDevice*                         render_device = device_.get();
  if (use_session_cache) {
    prepared_lease.emplace(source_cache_.AcquireEncoded(encoded_bytes, decode_res));
    plan = plan_cache_.GetOrCompile(*document_, prepared_lease->Get().CompileSource());
  } else {
    one_shot_prepared.emplace(unpack_(encoded_bytes, decode_res));
    plan          = GraphCompiler::CompileStatic(*document_, one_shot_prepared->CompileSource(),
                                                 kCudaDagBackendCapabilityVersion);
    render_device = one_shot_device_.get();
  }
  GraphCompiler::BindFrameGeometry(plan, *document_, request);
  if (document_->TopologyDirty()) {
    document_->ClearTopologyDirty();
  }
  const auto& prepared  = use_session_cache ? prepared_lease->Get() : *one_shot_prepared;
  const auto  output_id =
      render_device->Execute(plan, prepared, *document_, mask_store_.get(), false);
  const auto  release_one_shot_resources = [&]() {
    if (use_session_cache) {
      return;
    }
    render_device->Workspace().Images().DiscardUnpublished();
    render_device->WaitIdle();
    render_device->Workspace().ReleaseSessionResources();
  };

  ViewerDisplayConfig display_config{};
  if (const auto* drt = document_->Drt()) {
    display_config = ViewerDisplayConfigFromDrt(drt->Params().Params());
  }

  try {
    if (sink != nullptr) {
      PresentCudaTexture(*render_device, output_id, *sink, submission, display_config);
    }
    if (require_host_output) {
      auto host = DownloadCudaTexture(*render_device, output_id);
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
    render_device->Workspace().Images().DiscardUnpublished();
    render_device->ReportError(ex.what());
    throw;
  }
  return std::make_shared<ImageBuffer>();
}

auto CudaProductRenderer::Stats() const -> CudaProductSessionStats {
  const auto              source = source_cache_.GetStats();
  const auto              plan   = plan_cache_.GetStats();
  CudaProductSessionStats stats;
  stats.prepared_source_hits     = source.hits;
  stats.prepared_source_misses   = source.misses;
  stats.libraw_open_unpack_count = source.libraw_open_unpack_count;
  stats.plan_cache_hits          = plan.hits;
  stats.plan_cache_misses        = plan.misses;
  stats.plan_compile_count       = plan.compiles;
  stats.pass                     = device_->PassStats();
  return stats;
}

void CudaProductRenderer::ResetStats() {
  source_cache_.ResetStats();
  plan_cache_.ResetStats();
  device_->ResetPassStats();
}

void CudaProductRenderer::ReleaseSessionCaches() {
  if (!device_) {
    return;
  }
  device_->WaitIdle();
  device_->Workspace().ReleaseSessionResources();
  device_->ReleaseNeuralDemosaicWorkspace();
  one_shot_device_.reset();
  source_cache_.Clear();
  plan_cache_.Clear();
  device_->ResetPassStats();
}

auto CudaProductRenderer::SessionResources() const -> CudaProductSessionResources {
  CudaProductSessionResources resources;
  if (!device_) {
    return resources;
  }
  const auto& workspace                 = device_->Workspace();
  resources.published_result_count      = workspace.Images().PublishedCount();
  resources.texture_pool_used_bytes     = workspace.Textures().UsedBytes();
  resources.texture_pool_entry_count    = workspace.Textures().EntryCount();
  resources.prepared_source_host_bytes  = source_cache_.HostBytesUsed();
  resources.prepared_source_entry_count = source_cache_.EntryCount();
  resources.session_value_ids           = workspace.Images().CurrentValueIds();
  return resources;
}

}  // namespace alcedo
