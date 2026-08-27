//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>
#include <opencv2/core/types.hpp>

#include <algorithm>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/metal_demosaicnet_tiled.hpp"
#include "decoders/processor/operators/gpu/metal_encode.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/camera_color_gpu_params.hpp"
#include "edit/runtime/develop_demosaic.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

MetalDemosaicNetModelCache* g_metal_neural_cache_for_test = nullptr;

struct GeometryResampleParams {
  float         m00;
  float         m01;
  float         m02;
  float         m10;
  float         m11;
  float         m12;
  std::uint32_t decoded_width;
  std::uint32_t decoded_height;
  std::uint32_t render_width;
  std::uint32_t render_height;
  float         border[4];
  std::uint32_t use_bicubic;
};

auto AcquireRgba(MetalRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<MetalBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto AcquireScratch(MetalRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height,
                    TextureFormat format) -> ResourceLease<MetalBackend> {
  return workspace.Textures().Acquire({width, height, format});
}

auto CommandBuffer(MetalRenderDevice& device) -> void* {
  device.Workspace().Device().EndCommandEncoders(device.CommandContext());
  auto* buffer = device.CommandContext().NativeCommandBuffer();
  if (buffer == nullptr) {
    (void)device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext());
    device.Workspace().Device().EndCommandEncoders(device.CommandContext());
    buffer = device.CommandContext().NativeCommandBuffer();
  }
  if (buffer == nullptr) {
    throw std::runtime_error("ExecuteMetalDevelop: command buffer is missing");
  }
  return buffer;
}

auto Native(const MetalBackend::Texture2D& texture) -> void* { return texture.Native(); }

void DispatchGeometryResample(void* command_buffer, const ResolvedRenderGeometry& geometry,
                              const MetalBackend::Texture2D& src, MetalBackend::Texture2D& dst) {
#ifndef ALCEDO_METAL_GEOMETRY_RESAMPLE_METALLIB_PATH
  throw std::runtime_error("Metal geometry resample metallib path is not configured.");
#else
  auto pipeline = metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_GEOMETRY_RESAMPLE_METALLIB_PATH, "geometry_resample_rgba32f",
      "Metal GeometryResample");
#endif
  const auto&            gpu = geometry.gpu_data;
  GeometryResampleParams params{
      .m00            = gpu.render_to_decoded[0],
      .m01            = gpu.render_to_decoded[1],
      .m02            = gpu.render_to_decoded[2],
      .m10            = gpu.render_to_decoded[3],
      .m11            = gpu.render_to_decoded[4],
      .m12            = gpu.render_to_decoded[5],
      .decoded_width  = gpu.decoded_width,
      .decoded_height = gpu.decoded_height,
      .render_width   = gpu.render_width,
      .render_height  = gpu.render_height,
      .border = {gpu.border_rgba[0], gpu.border_rgba[1], gpu.border_rgba[2], gpu.border_rgba[3]},
      .use_bicubic = geometry.filter == TextureFilter::Bicubic ? 1U : 0U,
  };
  auto* buffer  = static_cast<MTL::CommandBuffer*>(command_buffer);
  auto  compute = NS::RetainPtr(buffer->computeCommandEncoder());
  if (!compute) {
    throw std::runtime_error("ExecuteMetalGeometryResample: failed to create compute encoder");
  }
  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  compute->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  compute->setBytes(&params, sizeof(params), 0);
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  compute->dispatchThreads(MTL::Size{gpu.render_width, gpu.render_height, 1},
                           MTL::Size{thread_width, thread_height, 1});
  compute->endEncoding();
}

void DispatchCameraColor(void* command_buffer, const MetalBackend::Texture2D& src,
                         MetalBackend::Texture2D& dst, const MetalBackend::Buffer& params,
                         std::uint32_t offset) {
#ifndef ALCEDO_METAL_CAMERA_COLOR_METALLIB_PATH
  throw std::runtime_error("Metal camera color metallib path is not configured.");
#else
  auto pipeline = metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_CAMERA_COLOR_METALLIB_PATH, "camera_color_acescc", "Metal CameraColor");
#endif
  auto* buffer  = static_cast<MTL::CommandBuffer*>(command_buffer);
  auto  compute = NS::RetainPtr(buffer->computeCommandEncoder());
  if (!compute) {
    throw std::runtime_error("ExecuteMetalCameraColor: failed to create compute encoder");
  }
  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  compute->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  compute->setBuffer(static_cast<MTL::Buffer*>(params.Native()), offset, 0);
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  compute->dispatchThreads(MTL::Size{src.Width(), src.Height(), 1},
                           MTL::Size{thread_width, thread_height, 1});
  compute->endEncoding();
}

auto CropOrFull(const PreparedRawInput& input, std::uint32_t width, std::uint32_t height) -> RectI {
  if (input.demosaic_output_crop.width > 0 && input.demosaic_output_crop.height > 0) {
    return input.demosaic_output_crop;
  }
  return RectI{0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

void EncodeNeural(MetalRenderDevice& device, void* command_buffer, const PreparedRawInput& input,
                  ResourceLease<MetalBackend>& linear, ResourceLease<MetalBackend>& packed,
                  bool hlr) {
  std::string error;
  const auto  geometry =
      ComputeNeuralAlignedGeometry(input.cfa_pattern, static_cast<int>(linear.Texture().Width()),
                                   static_cast<int>(linear.Texture().Height()), 32, &error);
  if (!geometry.has_value()) {
    throw std::runtime_error("ExecuteMetalDevelop: Neural Engine preprocess failed: " + error);
  }
  const RectI crop       = CropOrFull(input, static_cast<std::uint32_t>(geometry->aligned_width),
                                      static_cast<std::uint32_t>(geometry->aligned_height));
  const auto  out_w      = static_cast<std::uint32_t>(crop.width);
  const auto  out_h      = static_cast<std::uint32_t>(crop.height);
  auto        neural_out = AcquireScratch(device.Workspace(), out_w, out_h, TextureFormat::Rgba32f);
  auto cfa_image = metal::MetalImage::Wrap(static_cast<MTL::Texture*>(linear.Texture().Native()));
  auto out_image =
      metal::MetalImage::Wrap(static_cast<MTL::Texture*>(neural_out.Texture().Native()));

  MetalDemosaicNetLoadOptions load_options;
  if (g_metal_neural_cache_for_test != nullptr) {
    load_options.model_dir = "alcedo-missing-demosaicnet-models";
  }
  MetalDemosaicNetModelCache& cache    = g_metal_neural_cache_for_test == nullptr
                                             ? MetalDemosaicNetModelCache::Instance()
                                             : *g_metal_neural_cache_for_test;
  const bool                  is_bayer = input.cfa_pattern.kind == RawCfaKind::Bayer2x2;
  const auto variant = is_bayer ? MetalDemosaicNetVariant::Bayer : MetalDemosaicNetVariant::XTrans;
  if (!cache.EnsureLoaded(variant, load_options)) {
    throw std::runtime_error(std::string("ExecuteMetalDevelop: Neural Engine unavailable: ") +
                             cache.LastError());
  }

  MetalDemosaicNetTiledDispatch dispatch;
  dispatch.cfa_image       = &cfa_image;
  dispatch.output_rgba     = &out_image;
  dispatch.shift_sx        = geometry->shift_sx;
  dispatch.shift_sy        = geometry->shift_sy;
  dispatch.aligned_width   = geometry->aligned_width;
  dispatch.aligned_height  = geometry->aligned_height;
  dispatch.product_crop    = cv::Rect(crop.x, crop.y, crop.width, crop.height);
  dispatch.commit_and_wait = false;
  dispatch.command_buffer  = command_buffer;
  MetalDemosaicNetTiledExecutor executor;
  if (is_bayer) {
    (void)executor.EnqueueBayer(cache.Bayer(), dispatch);
  } else {
    (void)executor.EnqueueXTrans(cache.XTrans(), dispatch);
  }

  auto* hlr_src = Native(neural_out.Texture());
  auto  hlr_w   = neural_out.Texture().Width();
  auto  hlr_h   = neural_out.Texture().Height();
  if (hlr) {
    auto  hlr_dst = AcquireScratch(device.Workspace(), hlr_w, hlr_h, TextureFormat::Rgba32f);
    void* stats   = device.Workspace().TransientBuffers().Allocate(6 * sizeof(float));
    auto [native_stats, offset] =
        device.Workspace().Device().ResolveDeviceMemory(stats, 6 * sizeof(float));
    device.Workspace().Device().FillDeviceMemory(stats, 6 * sizeof(float), 0,
                                                 device.CommandContext());
    device.Workspace().Device().EndCommandEncoders(device.CommandContext());
    metal::EncodeHighlightReconstruct(command_buffer, hlr_src, Native(hlr_dst.Texture()),
                                      native_stats, offset, input.linearization.cam_mul, hlr_w,
                                      hlr_h);
    hlr_src = Native(hlr_dst.Texture());
  }
  const float* cam_mul = input.linearization.cam_mul;
  metal::EncodeCopyRgbaCropInverseOrient(
      command_buffer, hlr_src, Native(packed.Texture()),
      RectI{0, 0, static_cast<int>(hlr_w), static_cast<int>(hlr_h)}, cam_mul,
      input.sensor.orientation_flip);
}

void EncodeLegacyDemosaic(MetalRenderDevice& device, void* command_buffer,
                          const PreparedRawInput& input, ResourceLease<MetalBackend>& linear,
                          ResourceLease<MetalBackend>& packed, bool hlr) {
  const auto width  = linear.Texture().Width();
  const auto height = linear.Texture().Height();
  if (input.cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    auto      green  = AcquireScratch(device.Workspace(), width, height, TextureFormat::R32f);
    auto      rgb    = AcquireScratch(device.Workspace(), width, height, TextureFormat::Rgba32f);
    const int passes = input.downsample_passes == 0 ? 3 : 1;
    metal::EncodeXTrans(command_buffer, Native(linear.Texture()), Native(green.Texture()),
                        Native(rgb.Texture()), input.cfa_pattern.xtrans_pattern, width, height,
                        passes);
    auto* src = Native(rgb.Texture());
    if (hlr) {
      auto  hlr_dst = AcquireScratch(device.Workspace(), width, height, TextureFormat::Rgba32f);
      void* stats   = device.Workspace().TransientBuffers().Allocate(6 * sizeof(float));
      auto [native_stats, offset] =
          device.Workspace().Device().ResolveDeviceMemory(stats, 6 * sizeof(float));
      device.Workspace().Device().FillDeviceMemory(stats, 6 * sizeof(float), 0,
                                                   device.CommandContext());
      device.Workspace().Device().EndCommandEncoders(device.CommandContext());
      metal::EncodeHighlightReconstruct(command_buffer, src, Native(hlr_dst.Texture()),
                                        native_stats, offset, input.linearization.cam_mul, width,
                                        height);
      src = Native(hlr_dst.Texture());
    }
    metal::EncodeCopyRgbaCropInverseOrient(
        command_buffer, src, Native(packed.Texture()), CropOrFull(input, width, height),
        input.linearization.cam_mul, input.sensor.orientation_flip);
    return;
  }

  auto r  = AcquireScratch(device.Workspace(), width, height, TextureFormat::R32f);
  auto g  = AcquireScratch(device.Workspace(), width, height, TextureFormat::R32f);
  auto b  = AcquireScratch(device.Workspace(), width, height, TextureFormat::R32f);
  auto vh = AcquireScratch(device.Workspace(), width, height, TextureFormat::R32f);
  auto pq = AcquireScratch(device.Workspace(), width, height, TextureFormat::R32f);
  metal::EncodeBayerRcd(command_buffer, Native(linear.Texture()), Native(r.Texture()),
                        Native(g.Texture()), Native(b.Texture()), Native(vh.Texture()),
                        Native(pq.Texture()), input.cfa_pattern.bayer_pattern, width, height);
  if (hlr) {
    auto        rgba = AcquireScratch(device.Workspace(), width, height, TextureFormat::Rgba32f);
    const float identity[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    metal::EncodePackPlanesCropInverseOrient(
        command_buffer, Native(r.Texture()), Native(g.Texture()), Native(b.Texture()),
        Native(rgba.Texture()), RectI{0, 0, static_cast<int>(width), static_cast<int>(height)},
        identity, 0);
    auto  hlr_dst = AcquireScratch(device.Workspace(), width, height, TextureFormat::Rgba32f);
    void* stats   = device.Workspace().TransientBuffers().Allocate(6 * sizeof(float));
    auto [native_stats, offset] =
        device.Workspace().Device().ResolveDeviceMemory(stats, 6 * sizeof(float));
    device.Workspace().Device().FillDeviceMemory(stats, 6 * sizeof(float), 0,
                                                 device.CommandContext());
    device.Workspace().Device().EndCommandEncoders(device.CommandContext());
    metal::EncodeHighlightReconstruct(command_buffer, Native(rgba.Texture()),
                                      Native(hlr_dst.Texture()), native_stats, offset,
                                      input.linearization.cam_mul, width, height);
    metal::EncodeCopyRgbaCropInverseOrient(
        command_buffer, Native(hlr_dst.Texture()), Native(packed.Texture()),
        CropOrFull(input, width, height), input.linearization.cam_mul,
        input.sensor.orientation_flip);
    return;
  }
  metal::EncodePackPlanesCropInverseOrient(
      command_buffer, Native(r.Texture()), Native(g.Texture()), Native(b.Texture()),
      Native(packed.Texture()), CropOrFull(input, width, height), input.linearization.cam_mul,
      input.sensor.orientation_flip);
}

}  // namespace

void SetMetalDevelopNeuralModelCacheForTesting(MetalDemosaicNetModelCache* cache) {
  g_metal_neural_cache_for_test = cache;
}

void WarmUpMetalDagPlan(MetalBackend& backend, const ExecutionPlan& plan) {
  std::vector<MetalPipelineWarmup> pipelines;
  auto add = [&](const char* path, const char* function, const char* label) {
    if (path != nullptr && path[0] != '\0') {
      pipelines.push_back(MetalPipelineWarmup{path, function, label});
    }
  };
#ifdef ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH
  add(ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH, "to_linear_ref_r16u", "Metal ToLinearRef");
  add(ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH, "cfa_clamp01_r32f", "Metal CFA Clamp01");
#endif
#ifdef ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH
  add(ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH, "rcd_init_and_vh", "Metal Debayer RCD");
  add(ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH, "rcd_green_at_rb", "Metal Debayer RCD");
  add(ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH, "rcd_pq_dir", "Metal Debayer RCD");
  add(ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH, "rcd_rb_at_rb", "Metal Debayer RCD");
  add(ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH, "rcd_rb_at_g", "Metal Debayer RCD");
#endif
#ifdef ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH
  add(ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH, "xtrans_green", "Metal X-Trans interpolate");
  add(ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH, "xtrans_rgba", "Metal X-Trans interpolate");
#endif
#ifdef ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH
  add(ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH, "hlr_accumulate_stats",
      "Metal HighlightReconstruct");
  add(ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH, "hlr_reconstruct_tex",
      "Metal HighlightReconstruct");
#endif
#ifdef ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH
  add(ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH, "pack_planes_crop_inverse_orient", "Metal pack");
  add(ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH, "copy_rgba_crop_inverse_orient", "Metal pack");
#endif
#ifdef ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH
  add(ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH, "warp_rectilinear_tex_rgba32f",
      "Metal geometry utils");
#endif
#ifdef ALCEDO_METAL_GEOMETRY_RESAMPLE_METALLIB_PATH
  add(ALCEDO_METAL_GEOMETRY_RESAMPLE_METALLIB_PATH, "geometry_resample_rgba32f",
      "Metal GeometryResample");
#endif
#ifdef ALCEDO_METAL_CAMERA_COLOR_METALLIB_PATH
  add(ALCEDO_METAL_CAMERA_COLOR_METALLIB_PATH, "camera_color_acescc", "Metal CameraColor");
#endif
  AppendMetalPrimaryGradeWarmup(pipelines);
  AppendMetalMaskWarmup(pipelines);
  (void)plan;
  backend.WarmUpPipelines(pipelines);
}

void ExecuteMetalDevelop(MetalRenderDevice& device, const ExecutionPlan& plan,
                         const PreparedRawInput& input, PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteMetalDevelop: BeginRender has not been called");
  }
  auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteMetalDevelop: missing develop node");
  }
  auto pending = TakePendingParameterPatch(develop->Params());
  if (workspace.Textures().ByteBudget() == 0) {
    workspace.Textures().SetByteBudget(MetalBackend::DefaultTextureBudgetBytes());
  }
  if (plan.peak_transient_bytes > 0) {
    workspace.TransientBuffers().Reserve(plan.peak_transient_bytes);
  }

  const auto         flags         = develop->Params().Params();
  const bool         hlr           = flags.highlights_reconstruct;
  const auto         out_w         = plan.source.develop_output_extent.width;
  const auto         out_h         = plan.source.develop_output_extent.height;
  const GraphValueId sensor_id     = plan.sensor_linear_output;
  const GraphValueId demosaic_id   = input.dng_warp_rectilinear.has_value()
                                         ? GraphValueId{NodeId{"develop"}, PortId{"sensor_unwarped"}}
                                         : sensor_id;
  auto&              decoded_lease = AcquireRgba(workspace, demosaic_id, out_w, out_h);

  if (input.input_kind == RawInputKind::DebayeredRgb ||
      plan.source.kind == DevelopInputKind::DirectRgb) {
    if (input.pixels.format != HostPixelFormat::F32Rgba ||
        input.pixels.ByteCount() != decoded_lease.Texture().Bytes()) {
      throw std::runtime_error("ExecuteMetalDevelop: direct RGB size does not match output");
    }
    workspace.Device().UploadTexture2D(decoded_lease.Texture(), input.pixels.Span(),
                                       device.CommandContext());
    if (pending.has_value()) {
      pending->Commit();
    }
    return;
  }

  const auto width  = input.host_extent.width;
  const auto height = input.host_extent.height;
  if (width == 0 || height == 0) {
    throw std::runtime_error("ExecuteMetalDevelop: empty CFA");
  }
  if (input.pixels.stride_bytes != width * sizeof(std::uint16_t)) {
    throw std::runtime_error("ExecuteMetalDevelop: CFA plane must be tightly packed");
  }

  auto cfa    = AcquireScratch(workspace, width, height, TextureFormat::R16u);
  auto linear = AcquireScratch(workspace, width, height, TextureFormat::R32f);
  workspace.Device().UploadTexture2D(cfa.Texture(), input.pixels.Span(), device.CommandContext());
  auto* command_buffer = CommandBuffer(device);
  metal::EncodeToLinearRef(command_buffer, Native(cfa.Texture()), Native(linear.Texture()),
                           input.linearization, input.cfa_pattern);
  if (!hlr) {
    metal::EncodeCfaClamp01(command_buffer, Native(linear.Texture()), width, height);
  }

  const auto method =
      ResolveDevelopDemosaicMethod(flags, input.cfa_pattern.kind, input.downsample_passes);
  if (method == RawDemosaicMethod::NeuralEngine) {
    EncodeNeural(device, command_buffer, input, linear, decoded_lease, hlr);
  } else {
    EncodeLegacyDemosaic(device, command_buffer, input, linear, decoded_lease, hlr);
  }

  if (input.dng_warp_rectilinear.has_value()) {
    auto& warped = AcquireRgba(workspace, sensor_id, out_w, out_h);
    auto* source = workspace.Images().Find(demosaic_id);
    if (source == nullptr) {
      throw std::runtime_error("ExecuteMetalDevelop: DNG warp source was lost");
    }
    command_buffer = CommandBuffer(device);
    metal::EncodeWarpRectilinear(command_buffer, Native(source->Texture()),
                                 Native(warped.Texture()), *input.dng_warp_rectilinear, out_w,
                                 out_h);
  }

  if (pending.has_value()) {
    pending->Commit();
  }
}

void ExecuteMetalGeometryResample(MetalRenderDevice& device, const ExecutionPlan& plan) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteMetalGeometryResample: BeginRender has not been called");
  }
  auto* sensor = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr || sensor->Empty()) {
    throw std::runtime_error("ExecuteMetalGeometryResample: missing develop.sensor_linear");
  }
  auto& dest = AcquireRgba(workspace, plan.geometry_output, plan.geometry.render_extent.width,
                           plan.geometry.render_extent.height);
  sensor     = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr) {
    throw std::runtime_error("ExecuteMetalGeometryResample: sensor texture lost during acquire");
  }
  if (!plan.encode_geometry_resample) {
    workspace.Device().CopyTexture2D(sensor->Texture(), dest.Texture(), device.CommandContext());
    return;
  }
  auto* command_buffer = CommandBuffer(device);
  DispatchGeometryResample(command_buffer, plan.geometry, sensor->Texture(), dest.Texture());
}

void ExecuteMetalCameraColor(MetalRenderDevice& device, const ExecutionPlan& plan,
                             const PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteMetalCameraColor: BeginRender has not been called");
  }
  const auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteMetalCameraColor: missing develop node");
  }
  const auto resolved = ResolveDevelopColorTransform(develop->Params().Params());
  if (!resolved.ok) {
    throw std::runtime_error(std::string("ExecuteMetalCameraColor: ") +
                             std::string(ColorTransformErrorMessage(resolved.error)));
  }
  auto* input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteMetalCameraColor: missing geometry.scene_source");
  }
  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  auto&      output =
      workspace.AcquireImageForWrite(plan.develop_output, {width, height, TextureFormat::Rgba32f});
  input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr) {
    throw std::runtime_error("ExecuteMetalCameraColor: geometry texture lost during acquire");
  }

  CameraColorGpuParams gpu_params;
  for (int i = 0; i < 9; ++i) {
    gpu_params.camera_to_ap1[i] = resolved.transform.camera_to_ap1[static_cast<std::size_t>(i)];
  }
  auto&                       arena = workspace.Parameters();
  const ParameterSlotKey      key{develop->Id(), kDevelopCameraColorSlot};
  const ParameterFieldBinding field{DirtyFieldMask{DevelopDirty::WhiteBalance}, 0, 0,
                                    sizeof(CameraColorGpuParams)};
  auto payload = std::make_shared<TypedOperatorParamPayload<CameraColorGpuParams>>(
      type_ids::DevelopNode(), 1, gpu_params);
  if (!arena.Contains(key)) {
    arena.BindSlot(key, sizeof(CameraColorGpuParams), std::span{&field, 1});
    arena.InitializeFromFullDto(key, OperatorParamDto{type_ids::DevelopNode(), 1, payload});
  } else {
    OperatorParamPatchDto patch{develop->Id(), kDevelopCameraColorSlot, type_ids::DevelopNode(),
                                DirtyFieldMask{DevelopDirty::WhiteBalance}, payload};
    arena.ApplyPatch(key, patch);
  }
  arena.UploadDirty(device.CommandContext());
  const auto binding        = arena.Binding(key);
  auto*      command_buffer = CommandBuffer(device);
  DispatchCameraColor(command_buffer, input->Texture(), output.Texture(), arena.DeviceBuffer(),
                      binding.offset);
}

void ExecuteMetalIdentityCopy(MetalRenderDevice& device, const GraphValueId& source,
                              const GraphValueId& destination, ImageExtent extent) {
  auto& workspace = device.Workspace();
  auto* input     = workspace.Images().Find(source);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteMetalIdentityCopy: missing source texture");
  }
  auto& output = workspace.AcquireImageForWrite(
      destination, {extent.width, extent.height, TextureFormat::Rgba32f});
  input = workspace.Images().Find(source);
  if (input == nullptr) {
    throw std::runtime_error("ExecuteMetalIdentityCopy: source texture lost during acquire");
  }
  workspace.Device().CopyTexture2D(input->Texture(), output.Texture(), device.CommandContext());
}

}  // namespace alcedo
