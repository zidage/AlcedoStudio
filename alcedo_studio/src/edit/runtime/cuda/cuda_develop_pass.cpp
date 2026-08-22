//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_develop_pass.hpp"

#include <cstdint>
#include <stdexcept>

#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>

#include "decoders/processor/operators/gpu/cuda_color_space_conv.hpp"
#include "decoders/processor/operators/gpu/cuda_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/cuda_highlight_reconstruct.hpp"
#include "decoders/processor/operators/gpu/cuda_image_ops.hpp"
#include "decoders/processor/operators/gpu/cuda_white_balance.hpp"
#include "decoders/processor/operators/gpu/cuda_xtrans_interpolate.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
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

auto WrapF32C3(void* ptr, int width, int height) -> cv::cuda::GpuMat {
  return cv::cuda::GpuMat(height, width, CV_32FC3, ptr,
                          static_cast<std::size_t>(width) * sizeof(float) * 3);
}

auto WrapF32C4(void* ptr, int width, int height) -> cv::cuda::GpuMat {
  return cv::cuda::GpuMat(height, width, CV_32FC4, ptr,
                          static_cast<std::size_t>(width) * sizeof(float) * 4);
}

auto AllocateTransient(CudaRenderWorkspace& workspace, std::size_t bytes) -> void* {
  return workspace.TransientBuffers().Allocate(bytes);
}

auto EnsureImage(CudaRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<CudaBackend>& {
  auto* existing = workspace.Images().Find(id);
  if (existing != nullptr && !existing->Empty()) {
    const auto& tex = existing->Texture();
    if (tex.Width() == width && tex.Height() == height && tex.Format() == TextureFormat::Rgba32f) {
      return *existing;
    }
  }
  auto lease = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
  workspace.Images().Store(id, std::move(lease));
  auto* stored = workspace.Images().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteCudaDevelop: failed to store develop image");
  }
  return *stored;
}

auto CropIfNeeded(cv::cuda::GpuMat plane, const RectI& crop) -> cv::cuda::GpuMat {
  if (crop.width <= 0 || crop.height <= 0) {
    return plane;
  }
  if (crop.x == 0 && crop.y == 0 && crop.width == plane.cols && crop.height == plane.rows) {
    return plane;
  }
  return plane(cv::Rect(crop.x, crop.y, crop.width, crop.height));
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
    workspace.Textures().SetByteBudget(64ull << 20);
  }
  if (plan.peak_transient_bytes > 0) {
    workspace.TransientBuffers().Reserve(plan.peak_transient_bytes);
  }

  auto&        ctx    = device.CommandContext();
  auto         stream = WrapStream(ctx.Stream());
  const auto   flags  = develop->Params().Params();
  const bool   hlr    = flags.highlights_reconstruct;
  const GraphValueId develop_id = plan.develop_output;
  const auto   out_w  = plan.source.develop_output_extent.width;
  const auto   out_h  = plan.source.develop_output_extent.height;
  auto&        out_lease = EnsureImage(workspace, develop_id, out_w, out_h);
  auto&        out_tex   = out_lease.Texture();

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

  void* u16_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(std::uint16_t));
  void* f32_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
  workspace.Device().UploadDeviceMemory(u16_ptr, input.pixels.Span(), ctx);

  auto src_u16 = WrapU16(u16_ptr, w, h);
  auto linear  = WrapF32C1(f32_ptr, w, h);
  CUDA::ToLinearRef(src_u16, linear, input.linearization, input.cfa_pattern, &stream);

  const bool xtrans = input.cfa_pattern.kind == RawCfaKind::XTrans6x6;
  if (!hlr || xtrans) {
    CUDA::Clamp01(linear, &stream);
  }

  cv::cuda::GpuMat packed = WrapF32C4(out_tex.DevicePointer(), static_cast<int>(out_w),
                                      static_cast<int>(out_h));

  if (xtrans) {
    void* green_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
    void* rgb_ptr =
        AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float) * 3);
    auto green  = WrapF32C1(green_ptr, w, h);
    auto rgb    = WrapF32C3(rgb_ptr, w, h);
    const int passes = input.downsample_passes == 0 ? 3 : 1;
    CUDA::XTransToRGB_Ref(linear, green, rgb, input.cfa_pattern.xtrans_pattern, passes, &stream);
    auto cropped = CropIfNeeded(rgb, input.demosaic_output_crop);
    if (input.sensor.orientation_flip == 0 &&
        (packed.cols != cropped.cols || packed.rows != cropped.rows)) {
      throw std::runtime_error("ExecuteCudaDevelop: X-Trans output extent does not match plan");
    }
    CUDA::ApplyInverseCamMulAndPackRGBAOriented(cropped, packed, input.linearization.cam_mul,
                                                input.sensor.orientation_flip, &stream);
  } else {
    void* r_ptr  = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
    void* g_ptr  = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
    void* b_ptr  = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
    void* vh_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
    void* pq_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));

    CUDA::RcdWorkspace rcd;
    rcd.BindExternal(r_ptr, g_ptr, b_ptr, vh_ptr, pq_ptr, cv::Size(w, h));
    CUDA::Bayer2x2ToPlanarRGB_RCD(linear, input.cfa_pattern.bayer_pattern, &rcd, &stream);

    auto r = CropIfNeeded(rcd.r, input.demosaic_output_crop);
    auto g = CropIfNeeded(rcd.g, input.demosaic_output_crop);
    auto b = CropIfNeeded(rcd.b, input.demosaic_output_crop);
    if (input.sensor.orientation_flip == 0 && (packed.cols != r.cols || packed.rows != r.rows)) {
      throw std::runtime_error("ExecuteCudaDevelop: Bayer output extent does not match plan");
    }

    if (hlr) {
      const int cw = r.cols;
      const int ch = r.rows;
      int*   anyclipped = static_cast<int*>(AllocateTransient(workspace, sizeof(int)));
      float* sums       = static_cast<float*>(AllocateTransient(workspace, sizeof(float) * 4));
      float* cnts       = static_cast<float*>(AllocateTransient(workspace, sizeof(float) * 4));
      void*  hlr_rgb    = AllocateTransient(workspace, static_cast<std::size_t>(cw) * ch * sizeof(float) * 3);
      CUDA::HighlightWorkspace highlight;
      highlight.BindExternal(anyclipped, sums, cnts, hlr_rgb, cw, ch);
      CUDA::HighlightCorrection   correction = CUDA::BuildHighlightCorrection(input.linearization.cam_mul);
      CUDA::HighlightAccumulation accumulation;
      CUDA::AccumulateHighlightStats(r, g, b, correction, cv::Rect{}, highlight, accumulation,
                                     &stream);
      CUDA::FinalizeHighlightCorrection(accumulation, correction);
      CUDA::ApplyHighlightCorrectionAndPackRGBAOriented(r, g, b, packed, correction,
                                                        input.linearization.cam_mul,
                                                        input.sensor.orientation_flip, &highlight,
                                                        &stream);
    } else {
      void* merge_ptr =
          AllocateTransient(workspace, static_cast<std::size_t>(r.cols) * r.rows * sizeof(float) * 3);
      auto merged = WrapF32C3(merge_ptr, r.cols, r.rows);
      CUDA::MergeRGB(r, g, b, merged, &stream);
      CUDA::ApplyInverseCamMulAndPackRGBAOriented(merged, packed, input.linearization.cam_mul,
                                                  input.sensor.orientation_flip, &stream);
    }
  }

  if (plan.encode_geometry_resample) {
    const auto rw = plan.geometry.render_extent.width;
    const auto rh = plan.geometry.render_extent.height;
    auto       dest = workspace.Textures().Acquire({rw, rh, TextureFormat::Rgba32f});
    GeometryResamplePass pass;
    pass.Encode(plan.geometry, out_tex, dest.Texture(), ctx);
    workspace.Images().Store(develop_id, std::move(dest));
  }

  if (pending.has_value()) {
    pending->Commit();
  }
}

}  // namespace alcedo
