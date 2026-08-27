//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_develop_pass.hpp"

#include <cstdint>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <stdexcept>

#include "decoders/processor/operators/gpu/cuda_color_space_conv.hpp"
#include "decoders/processor/operators/gpu/cuda_dng_warp.hpp"
#include "decoders/processor/operators/gpu/cuda_highlight_reconstruct.hpp"
#include "decoders/processor/operators/gpu/cuda_white_balance.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/cuda/cuda_sensor_demosaic.hpp"
#include "edit/runtime/cuda/geometry_resample_pass.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

auto WrapStream(cudaStream_t native) -> cv::cuda::Stream {
  return cv::cuda::StreamAccessor::wrapStream(native);
}

auto WrapU16(void* ptr, int width, int height) -> cv::cuda::GpuMat {
  return cv::cuda::GpuMat(height, width, CV_16UC1, ptr,
                          static_cast<std::size_t>(width) * sizeof(std::uint16_t));
}

auto WrapF32C1(void* ptr, int width, int height) -> cv::cuda::GpuMat {
  return cv::cuda::GpuMat(height, width, CV_32FC1, ptr,
                          static_cast<std::size_t>(width) * sizeof(float));
}

auto WrapF32C4(void* ptr, int width, int height) -> cv::cuda::GpuMat {
  return cv::cuda::GpuMat(height, width, CV_32FC4, ptr,
                          static_cast<std::size_t>(width) * sizeof(float) * 4);
}

auto AllocateTransient(CudaRenderWorkspace& workspace, std::size_t bytes) -> void* {
  return workspace.TransientBuffers().Allocate(bytes);
}

auto AcquireRgba(CudaRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<CudaBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

}  // namespace

void ExecuteCudaDevelop(CudaRenderDevice& device, const ExecutionPlan& plan,
                        const PreparedRawInput& input, PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaDevelop: BeginRender has not been called");
  }
  auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteCudaDevelop: missing develop node");
  }

  auto pending = TakePendingParameterPatch(develop->Params());

  if (workspace.Textures().ByteBudget() == 0) {
    workspace.Textures().SetByteBudget(DefaultProductTextureBudgetBytes());
  }
  if (plan.peak_transient_bytes > 0) {
    workspace.TransientBuffers().Reserve(plan.peak_transient_bytes);
  }

  auto&              ctx           = device.CommandContext();
  auto               stream        = WrapStream(ctx.Stream());
  const auto         flags         = develop->Params().Params();
  const bool         hlr           = flags.highlights_reconstruct;
  const GraphValueId sensor_id     = plan.sensor_linear_output;
  const GraphValueId demosaic_id   = input.dng_warp_rectilinear.has_value()
                                         ? GraphValueId{NodeId{"develop"}, PortId{"sensor_unwarped"}}
                                         : sensor_id;
  const auto         out_w         = plan.source.develop_output_extent.width;
  const auto         out_h         = plan.source.develop_output_extent.height;
  auto&              decoded_lease = AcquireRgba(workspace, demosaic_id, out_w, out_h);
  auto&              out_tex       = decoded_lease.Texture();

  if (input.input_kind == RawInputKind::DebayeredRgb ||
      plan.source.kind == DevelopInputKind::DirectRgb) {
    if (input.pixels.format != HostPixelFormat::F32Rgba ||
        input.pixels.ByteCount() != out_tex.Bytes()) {
      throw std::runtime_error("ExecuteCudaDevelop: direct RGB size does not match output");
    }
    workspace.Device().UploadTexture2D(out_tex, input.pixels.Span(), ctx);
    if (pending.has_value()) {
      pending->Commit();
    }
    return;
  }

  const int w = static_cast<int>(input.host_extent.width);
  const int h = static_cast<int>(input.host_extent.height);
  if (w <= 0 || h <= 0) {
    throw std::runtime_error("ExecuteCudaDevelop: empty CFA");
  }
  const std::uint32_t packed_stride = static_cast<std::uint32_t>(w) * sizeof(std::uint16_t);
  if (input.pixels.stride_bytes != packed_stride) {
    throw std::runtime_error("ExecuteCudaDevelop: CFA plane must be tightly packed");
  }

  void* u16_ptr =
      AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(std::uint16_t));
  void* f32_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
  workspace.Device().UploadDeviceMemory(u16_ptr, input.pixels.Span(), ctx);

  auto src_u16 = WrapU16(u16_ptr, w, h);
  auto linear  = WrapF32C1(f32_ptr, w, h);
  CUDA::ToLinearRef(src_u16, linear, input.linearization, input.cfa_pattern, &stream);

  if (!hlr) {
    CUDA::Clamp01(linear, &stream);
  }

  cv::cuda::GpuMat packed =
      WrapF32C4(out_tex.DevicePointer(), static_cast<int>(out_w), static_cast<int>(out_h));
  ExecuteCudaSensorDemosaicAndPack(device, input, flags, linear, packed, stream);

  if (input.dng_warp_rectilinear.has_value()) {
    auto& warped_lease = AcquireRgba(workspace, sensor_id, out_w, out_h);
    auto* unwarped     = workspace.Images().Find(demosaic_id);
    if (unwarped == nullptr) {
      throw std::runtime_error("ExecuteCudaDevelop: DNG warp source was lost");
    }
    auto source = WrapF32C4(unwarped->Texture().DevicePointer(), static_cast<int>(out_w),
                            static_cast<int>(out_h));
    auto warped = WrapF32C4(warped_lease.Texture().DevicePointer(), static_cast<int>(out_w),
                            static_cast<int>(out_h));
    CUDA::WarpDngRectilinear(source, warped, *input.dng_warp_rectilinear, &stream);
  }

  if (pending.has_value()) {
    pending->Commit();
  }
}

void ExecuteCudaGeometryResample(CudaRenderDevice& device, const ExecutionPlan& plan) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaGeometryResample: BeginRender has not been called");
  }
  auto* sensor = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr || sensor->Empty()) {
    throw std::runtime_error("ExecuteCudaGeometryResample: missing develop.sensor_linear");
  }
  if (!plan.encode_geometry_resample) {
    workspace.AliasImageFrom(plan.geometry_output, plan.sensor_linear_output);
    return;
  }
  const auto width  = plan.geometry.render_extent.width;
  const auto height = plan.geometry.render_extent.height;
  auto&      dest   = AcquireRgba(workspace, plan.geometry_output, width, height);
  sensor            = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr) {
    throw std::runtime_error("ExecuteCudaGeometryResample: sensor texture lost during acquire");
  }
  GeometryResamplePass pass;
  pass.Encode(plan.geometry, sensor->Texture(), dest.Texture(), device.CommandContext());
}

}  // namespace alcedo
