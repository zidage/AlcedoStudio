//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_develop_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_tiled.hpp"
#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "decoders/processor/operators/gpu/opencl_encode.hpp"
#include "decoders/dng_default_crop.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/camera_color_gpu_params.hpp"
#include "edit/runtime/develop_demosaic.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "opencl/nn/activation_slots.hpp"
#include "opencl/nn/device_buffer.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

OpenClDemosaicNetModelCache* g_opencl_neural_cache_for_test = nullptr;

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

struct ImageWarpParams {
  std::uint32_t coefficient_set_count = 0;
  std::uint32_t width                 = 0;
  std::uint32_t height                = 0;
  float         coefficient_sets[3][6]{};
  float         center_x = 0.5f;
  float         center_y = 0.5f;
};

struct OpenClNeuralSessionWorkspace {
  OpenClDemosaicNetTiledExecutor executor;
  opencl::nn::ActivationSlots    slots;
  opencl::nn::DeviceBuffer       linear_cfa;
  opencl::nn::DeviceBuffer       mosaic_hwc;
  opencl::nn::DeviceBuffer       rgb_hwc;
  opencl::nn::DeviceBuffer       rgba;
  opencl::nn::DeviceBuffer       cfa_table;

  void Reset() {
    executor  = {};
    slots     = {};
    linear_cfa.Reset();
    mosaic_hwc.Reset();
    rgb_hwc.Reset();
    rgba.Reset();
    cfa_table.Reset();
  }
};

auto NeuralWorkspace() -> OpenClNeuralSessionWorkspace& {
  static OpenClNeuralSessionWorkspace workspace;
  return workspace;
}

auto AcquireRgba(OpenClRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<OpenClBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto MakeEncodeQueue(OpenClRenderDevice& device) -> opencl::OpenClEncodeQueue {
  return opencl::OpenClEncodeQueue{
      .queue = device.Workspace().Device().NativeQueue(),
      .retain_event =
          [](cl_event event, void* context) {
            auto* render_device = static_cast<OpenClRenderDevice*>(context);
            render_device->Workspace().Device().TrackKernelEvent(render_device->CommandContext(),
                                                                 event);
          },
      .retain_ctx = &device,
  };
}

auto ViewFromPtr(OpenClBackend& backend, void* ptr, std::size_t bytes) -> opencl::OpenClBufferView {
  const auto [native, offset] = backend.ResolveDeviceMemory(ptr, bytes);
  return opencl::OpenClBufferView{native, offset};
}

auto CropOrFull(const PreparedRawInput& input, std::uint32_t width, std::uint32_t height) -> RectI {
  if (input.demosaic_output_crop.width > 0 && input.demosaic_output_crop.height > 0) {
    return input.demosaic_output_crop;
  }
  return RectI{0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

void DispatchKernel(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t width,
                    std::uint32_t height) {
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              "OpenCL DAG enqueue");
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

void EncodeWarp(OpenClRenderDevice& device, cl_mem src, cl_mem dst, const dng::WarpRectilinear& warp,
                std::uint32_t width, std::uint32_t height) {
  ImageWarpParams params{};
  params.coefficient_set_count = warp.coefficient_set_count;
  params.width                 = width;
  params.height                = height;
  params.center_x              = static_cast<float>(warp.center_x);
  params.center_y              = static_cast<float>(warp.center_y);
  for (std::uint32_t set = 0; set < 3; ++set) {
    for (int i = 0; i < 6; ++i) {
      params.coefficient_sets[set][i] = static_cast<float>(warp.coefficient_sets[set][i]);
    }
  }
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kGeometryCameraProgramName,
                                                        OpenCL::GpuDag::kWarpRectilinearKernelName);
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src), "warp arg0");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst), "warp arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "warp arg2");
  DispatchKernel(device, kernel, width, height);
}

void RewindDevelopScratch(OpenClRenderDevice& device) {
  device.Workspace().Device().SynchronizeRecordedWork(device.CommandContext());
  device.Workspace().TransientBuffers().Reset();
}

void EncodeHighlight(OpenClRenderDevice& device, opencl::OpenClEncodeQueue& stream,
                     opencl::OpenClBufferView src, opencl::OpenClBufferView dst,
                     std::uint32_t width, std::uint32_t height, const float* cam_mul) {
  auto&             backend    = device.Workspace().Device();
  auto&             transients = device.Workspace().TransientBuffers();
  const std::size_t mask_bytes = static_cast<std::size_t>(width) * height * 6;
  void*             mask_ptr    = transients.Allocate(mask_bytes);
  void*             dilated_ptr = transients.Allocate(mask_bytes);
  void*             sums_ptr    = transients.Allocate(4 * sizeof(float));
  void*             cnts_ptr    = transients.Allocate(4 * sizeof(float));
  void*             clipped_ptr = transients.Allocate(sizeof(cl_int));
  backend.FillDeviceMemory(mask_ptr, mask_bytes, 0, device.CommandContext());
  backend.FillDeviceMemory(dilated_ptr, mask_bytes, 0, device.CommandContext());
  backend.FillDeviceMemory(sums_ptr, 4 * sizeof(float), 0, device.CommandContext());
  backend.FillDeviceMemory(cnts_ptr, 4 * sizeof(float), 0, device.CommandContext());
  backend.FillDeviceMemory(clipped_ptr, sizeof(cl_int), 0, device.CommandContext());
  OpenCL::EncodeHighlightReconstruct(stream, src, dst, ViewFromPtr(backend, mask_ptr, mask_bytes),
                                     ViewFromPtr(backend, dilated_ptr, mask_bytes),
                                     ViewFromPtr(backend, sums_ptr, 4 * sizeof(float)),
                                     ViewFromPtr(backend, cnts_ptr, 4 * sizeof(float)),
                                     ViewFromPtr(backend, clipped_ptr, sizeof(cl_int)), cam_mul,
                                     width, height);
}

void CopyRgbaToPacked(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView src,
                      cl_mem packed, const PreparedRawInput& input, std::uint32_t width,
                      std::uint32_t height, bool apply_cam_mul) {
  const float identity[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  OpenCL::EncodeCopyRgbaCropInverseOrient(
      stream, src, packed, CropOrFull(input, width, height), width,
      apply_cam_mul ? input.linearization.cam_mul : identity, input.sensor.orientation_flip);
}

void EncodeHighlightFromRgbaImage(OpenClRenderDevice& device, opencl::OpenClEncodeQueue& stream,
                                  const OpenClBackend::Texture2D& src_image, cl_mem packed,
                                  const PreparedRawInput& input, std::uint32_t width,
                                  std::uint32_t height) {
  auto&      backend    = device.Workspace().Device();
  auto&      transients = device.Workspace().TransientBuffers();
  RewindDevelopScratch(device);
  const auto rgba_bytes = static_cast<std::size_t>(width) * height * 4 * sizeof(float);
  void*      src_ptr    = transients.Allocate(rgba_bytes);
  backend.CopyImageToDeviceMemory(src_image, src_ptr, rgba_bytes, device.CommandContext());
  void* dst_ptr = transients.Allocate(rgba_bytes);
  auto  src     = ViewFromPtr(backend, src_ptr, rgba_bytes);
  auto  dst     = ViewFromPtr(backend, dst_ptr, rgba_bytes);
  EncodeHighlight(device, stream, src, dst, width, height, input.linearization.cam_mul);
  CopyRgbaToPacked(stream, dst, packed, input, width, height, true);
}

void EncodeLegacyDemosaic(OpenClRenderDevice& device, opencl::OpenClEncodeQueue& stream,
                          const PreparedRawInput& input, opencl::OpenClBufferView linear,
                          cl_mem packed, bool hlr) {
  auto&      backend     = device.Workspace().Device();
  auto&      transients  = device.Workspace().TransientBuffers();
  const auto width       = input.host_extent.width;
  const auto height      = input.host_extent.height;
  const auto plane_bytes = static_cast<std::size_t>(width) * height * sizeof(float);
  const auto rgba_bytes  = plane_bytes * 4;

  if (input.cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    void* green_ptr = transients.Allocate(plane_bytes);
    void* rgba_ptr  = transients.Allocate(rgba_bytes);
    auto  rgba      = ViewFromPtr(backend, rgba_ptr, rgba_bytes);
    OpenCL::EncodeXTrans(stream, linear, ViewFromPtr(backend, green_ptr, plane_bytes), rgba,
                         input.cfa_pattern.xtrans_pattern, width, height,
                         input.downsample_passes == 0 ? 3 : 1);
    if (hlr) {
      auto rgba_image =
          device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
      backend.CopyDeviceMemoryToImage(rgba_ptr, rgba_image.Texture(), device.CommandContext());
      EncodeHighlightFromRgbaImage(device, stream, rgba_image.Texture(), packed, input, width,
                                   height);
      return;
    }
    CopyRgbaToPacked(stream, rgba, packed, input, width, height, true);
    return;
  }

  void* r_ptr  = transients.Allocate(plane_bytes);
  void* g_ptr  = transients.Allocate(plane_bytes);
  void* b_ptr  = transients.Allocate(plane_bytes);
  void* vh_ptr = transients.Allocate(plane_bytes);
  void* pq_ptr = transients.Allocate(plane_bytes);
  auto  r      = ViewFromPtr(backend, r_ptr, plane_bytes);
  auto  g      = ViewFromPtr(backend, g_ptr, plane_bytes);
  auto  b      = ViewFromPtr(backend, b_ptr, plane_bytes);
  OpenCL::EncodeBayerRcd(stream, linear, r, g, b, ViewFromPtr(backend, vh_ptr, plane_bytes),
                         ViewFromPtr(backend, pq_ptr, plane_bytes), input.cfa_pattern.bayer_pattern,
                         width, height);
  if (hlr) {
    const float identity[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    auto        rgba_image =
        device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
    OpenCL::EncodePackPlanesCropInverseOrient(
        stream, r, g, b, rgba_image.Texture().Native(),
        RectI{0, 0, static_cast<int>(width), static_cast<int>(height)}, width, identity, 0);
    EncodeHighlightFromRgbaImage(device, stream, rgba_image.Texture(), packed, input, width,
                                 height);
    return;
  }
  OpenCL::EncodePackPlanesCropInverseOrient(stream, r, g, b, packed,
                                            CropOrFull(input, width, height), width,
                                            input.linearization.cam_mul,
                                            input.sensor.orientation_flip);
}

void EncodeNeural(OpenClRenderDevice& device, opencl::OpenClEncodeQueue& stream,
                  const PreparedRawInput& input, opencl::OpenClBufferView linear, cl_mem packed,
                  bool hlr) {
  std::string error;
  const int   min_spatial = input.cfa_pattern.kind == RawCfaKind::XTrans6x6
                                ? DemosaicNetXTransSpec::kMinSpatial
                                : DemosaicNetBayerSpec::kMinSpatial;
  const auto geometry = ComputeNeuralAlignedGeometry(
      input.cfa_pattern, static_cast<int>(input.host_extent.width),
      static_cast<int>(input.host_extent.height), min_spatial, &error);
  if (!geometry.has_value()) {
    throw std::runtime_error("ExecuteOpenClDevelop: Neural Engine preprocess failed: " + error);
  }

  OpenClDemosaicNetLoadOptions load_options;
  if (g_opencl_neural_cache_for_test != nullptr) {
    load_options.model_dir = "alcedo-missing-demosaicnet-models";
  }
  OpenClDemosaicNetModelCache& cache = g_opencl_neural_cache_for_test == nullptr
                                           ? OpenClDemosaicNetModelCache::Instance()
                                           : *g_opencl_neural_cache_for_test;
  const bool is_bayer = input.cfa_pattern.kind == RawCfaKind::Bayer2x2;
  const auto variant =
      is_bayer ? OpenClDemosaicNetVariant::Bayer : OpenClDemosaicNetVariant::XTrans;
  load_options.queue = device.Workspace().Device().NativeQueue();
  if (!cache.EnsureLoaded(variant, load_options)) {
    throw std::runtime_error(std::string("ExecuteOpenClDevelop: Neural Engine unavailable: ") +
                             cache.LastError());
  }

  auto&       neural     = NeuralWorkspace();
  auto&       backend    = device.Workspace().Device();
  const auto  width      = input.host_extent.width;
  const auto  height     = input.host_extent.height;
  const auto  plane_bytes = static_cast<std::size_t>(width) * height * sizeof(float);
  neural.linear_cfa.EnsureBytes(plane_bytes);
  cl_event copy_event = nullptr;
  CheckOpenCl(clEnqueueCopyBuffer(backend.NativeQueue(), linear.native, neural.linear_cfa.get(),
                                  linear.offset_bytes, 0, plane_bytes, 0, nullptr, &copy_event),
              "OpenCL Neural linear copy");
  backend.TrackKernelEvent(device.CommandContext(), copy_event);

  const RawCfaPattern training = DemosaicNetTrainingPattern(input.cfa_pattern.kind);
  const int           period   = CfaPeriod(training.kind);
  std::vector<int>    rgb_fc(static_cast<std::size_t>(period) * period);
  for (int y = 0; y < period; ++y) {
    for (int x = 0; x < period; ++x) {
      rgb_fc[static_cast<std::size_t>(y * period + x)] = RgbColorAt(training, y, x);
    }
  }
  neural.cfa_table.EnsureBytes(rgb_fc.size() * sizeof(int));
  neural.cfa_table.UploadBytes(rgb_fc.data(), rgb_fc.size() * sizeof(int), backend.NativeQueue(),
                               false);

  const std::size_t hwc3 =
      static_cast<std::size_t>(geometry->aligned_width) * geometry->aligned_height * 3 *
      sizeof(float);
  neural.mosaic_hwc.EnsureBytes(hwc3);
  neural.rgb_hwc.EnsureBytes(hwc3);
  neural.rgba.EnsureBytes(static_cast<std::size_t>(geometry->aligned_width) *
                          geometry->aligned_height * 4 * sizeof(float));

  auto pack = OpenClKernelCache::Instance().GetKernel(OpenCL::DemosaicNet::kStructuralProgramName,
                                                      OpenCL::DemosaicNet::kPackCfaMonoToHwc3KernelName);
  cl_mem linear_mem = neural.linear_cfa.get();
  cl_mem mosaic_mem = neural.mosaic_hwc.get();
  cl_mem table_mem  = neural.cfa_table.get();
  const int src_w   = static_cast<int>(width);
  const int src_h   = static_cast<int>(height);
  CheckOpenCl(clSetKernelArg(pack, 0, sizeof(cl_mem), &linear_mem), "neural pack 0");
  CheckOpenCl(clSetKernelArg(pack, 1, sizeof(cl_mem), &mosaic_mem), "neural pack 1");
  CheckOpenCl(clSetKernelArg(pack, 2, sizeof(int), &src_w), "neural pack 2");
  CheckOpenCl(clSetKernelArg(pack, 3, sizeof(int), &src_h), "neural pack 3");
  CheckOpenCl(clSetKernelArg(pack, 4, sizeof(int), &geometry->shift_sx), "neural pack 4");
  CheckOpenCl(clSetKernelArg(pack, 5, sizeof(int), &geometry->shift_sy), "neural pack 5");
  CheckOpenCl(clSetKernelArg(pack, 6, sizeof(int), &geometry->aligned_width), "neural pack 6");
  CheckOpenCl(clSetKernelArg(pack, 7, sizeof(int), &geometry->aligned_height), "neural pack 7");
  CheckOpenCl(clSetKernelArg(pack, 8, sizeof(int), &period), "neural pack 8");
  CheckOpenCl(clSetKernelArg(pack, 9, sizeof(cl_mem), &table_mem), "neural pack 9");
  DispatchKernel(device, pack, static_cast<std::uint32_t>(geometry->aligned_width),
                 static_cast<std::uint32_t>(geometry->aligned_height));

  OpenClDemosaicNetTiledDispatch dispatch;
  dispatch.input_aligned_hwc  = neural.mosaic_hwc.get();
  dispatch.output_aligned_hwc = neural.rgb_hwc.get();
  dispatch.aligned_width      = geometry->aligned_width;
  dispatch.aligned_height     = geometry->aligned_height;
  dispatch.queue              = backend.NativeQueue();
  if (is_bayer) {
    (void)neural.executor.EnqueueBayer(cache.Bayer(), neural.slots, dispatch);
  } else {
    (void)neural.executor.EnqueueXTrans(cache.XTrans(), neural.slots, dispatch);
  }

  auto rgb_to_rgba = OpenClKernelCache::Instance().GetKernel(
      OpenCL::DemosaicNet::kStructuralProgramName, OpenCL::DemosaicNet::kRgb3ToRgba4KernelName);
  cl_mem rgb_mem  = neural.rgb_hwc.get();
  cl_mem rgba_mem = neural.rgba.get();
  CheckOpenCl(clSetKernelArg(rgb_to_rgba, 0, sizeof(cl_mem), &rgb_mem), "neural rgba 0");
  CheckOpenCl(clSetKernelArg(rgb_to_rgba, 1, sizeof(cl_mem), &rgba_mem), "neural rgba 1");
  CheckOpenCl(clSetKernelArg(rgb_to_rgba, 2, sizeof(int), &geometry->aligned_width), "neural rgba 2");
  CheckOpenCl(clSetKernelArg(rgb_to_rgba, 3, sizeof(int), &geometry->aligned_height), "neural rgba 3");
  DispatchKernel(device, rgb_to_rgba, static_cast<std::uint32_t>(geometry->aligned_width),
                 static_cast<std::uint32_t>(geometry->aligned_height));

  opencl::OpenClBufferView rgba{neural.rgba.get(), 0};
  const auto aligned_w = static_cast<std::uint32_t>(geometry->aligned_width);
  const auto aligned_h = static_cast<std::uint32_t>(geometry->aligned_height);
  if (hlr) {
    RewindDevelopScratch(device);
    const auto rgba_bytes =
        static_cast<std::size_t>(aligned_w) * aligned_h * 4 * sizeof(float);
    void* hlr_ptr = device.Workspace().TransientBuffers().Allocate(rgba_bytes);
    auto  hlr_dst = ViewFromPtr(backend, hlr_ptr, rgba_bytes);
    EncodeHighlight(device, stream, rgba, hlr_dst, aligned_w, aligned_h,
                    input.linearization.cam_mul);
    rgba = hlr_dst;
  }
  CopyRgbaToPacked(stream, rgba, packed, input, aligned_w, aligned_h, true);
}

}  // namespace

void SetOpenClDevelopNeuralModelCacheForTesting(OpenClDemosaicNetModelCache* cache) {
  g_opencl_neural_cache_for_test = cache;
}

void ReleaseOpenClDevelopNeuralWorkspace() { NeuralWorkspace().Reset(); }

void ExecuteOpenClDevelop(OpenClRenderDevice& device, const ExecutionPlan& plan,
                          const PreparedRawInput& input, PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClDevelop: BeginRender has not been called");
  }
  auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteOpenClDevelop: missing develop node");
  }
  auto pending = TakePendingParameterPatch(develop->Params());
  if (workspace.Textures().ByteBudget() == 0) {
    workspace.Textures().SetByteBudget(OpenClBackend::DefaultTextureBudgetBytes());
  }
  const auto flags = develop->Params().Params();
  const bool hlr   = flags.highlights_reconstruct;
  const auto host_pixels =
      static_cast<std::size_t>(input.host_extent.width) * input.host_extent.height;
  const bool cfa_input = input.input_kind != RawInputKind::DebayeredRgb &&
                         plan.source.kind != DevelopInputKind::DirectRgb;
  std::size_t opencl_need = plan.peak_transient_bytes;
  if (cfa_input && hlr && host_pixels > 0) {
    // Exclusive HLR working set after demosaic planes are rewound: RGBA src, RGBA dst,
    // 6-byte mask + dilated mask, and bump padding. Take the max with the compiled
    // peak; do not add this on top of LLF, which does not run concurrently.
    const auto hlr_bytes =
        host_pixels * 16ull + host_pixels * 16ull + host_pixels * 12ull + (64ull * 256ull);
    if (hlr_bytes > opencl_need) {
      opencl_need = hlr_bytes;
    }
  }
  if (opencl_need > 0) {
    workspace.TransientBuffers().Reserve(opencl_need);
  }
  const auto         out_w       = plan.source.develop_output_extent.width;
  const auto         out_h       = plan.source.develop_output_extent.height;
  const GraphValueId sensor_id   = plan.sensor_linear_output;
  const GraphValueId demosaic_id = input.dng_warp_rectilinear.has_value()
                                       ? GraphValueId{NodeId{"develop"}, PortId{"sensor_unwarped"}}
                                       : sensor_id;
  auto& decoded_lease = AcquireRgba(workspace, demosaic_id, out_w, out_h);

  if (input.input_kind == RawInputKind::DebayeredRgb ||
      plan.source.kind == DevelopInputKind::DirectRgb) {
    if (input.pixels.format != HostPixelFormat::F32Rgba ||
        input.pixels.ByteCount() != decoded_lease.Texture().Bytes()) {
      throw std::runtime_error("ExecuteOpenClDevelop: direct RGB size does not match output");
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
    throw std::runtime_error("ExecuteOpenClDevelop: empty CFA");
  }
  if (input.pixels.stride_bytes != width * sizeof(std::uint16_t)) {
    throw std::runtime_error("ExecuteOpenClDevelop: CFA plane must be tightly packed");
  }

  auto&       backend      = workspace.Device();
  auto&       transients   = workspace.TransientBuffers();
  const auto  u16_bytes    = static_cast<std::size_t>(width) * height * sizeof(std::uint16_t);
  const auto  f32_bytes    = static_cast<std::size_t>(width) * height * sizeof(float);
  void*       u16_ptr      = transients.Allocate(u16_bytes);
  void*       f32_ptr      = transients.Allocate(f32_bytes);
  backend.UploadDeviceMemory(u16_ptr, input.pixels.Span(), device.CommandContext());
  auto stream = MakeEncodeQueue(device);
  auto linear = ViewFromPtr(backend, f32_ptr, f32_bytes);
  OpenCL::EncodeToLinearRef(stream, ViewFromPtr(backend, u16_ptr, u16_bytes), linear, width, height,
                            input.linearization, input.cfa_pattern);
  if (!hlr) {
    OpenCL::EncodeCfaClamp01(stream, linear, width, height);
  }

  const auto method =
      ResolveDevelopDemosaicMethod(flags, input.cfa_pattern.kind, input.downsample_passes);
  if (method == RawDemosaicMethod::NeuralEngine) {
    EncodeNeural(device, stream, input, linear, decoded_lease.Texture().Native(), hlr);
  } else {
    EncodeLegacyDemosaic(device, stream, input, linear, decoded_lease.Texture().Native(), hlr);
  }

  if (input.dng_warp_rectilinear.has_value()) {
    auto& warped = AcquireRgba(workspace, sensor_id, out_w, out_h);
    auto* source = workspace.Images().Find(demosaic_id);
    if (source == nullptr) {
      throw std::runtime_error("ExecuteOpenClDevelop: DNG warp source was lost");
    }
    EncodeWarp(device, source->Texture().Native(), warped.Texture().Native(),
               *input.dng_warp_rectilinear, out_w, out_h);
  }

  if (pending.has_value()) {
    pending->Commit();
  }
}

void ExecuteOpenClGeometryResample(OpenClRenderDevice& device, const ExecutionPlan& plan) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClGeometryResample: BeginRender has not been called");
  }
  auto* sensor = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr || sensor->Empty()) {
    throw std::runtime_error("ExecuteOpenClGeometryResample: missing develop.sensor_linear");
  }
  if (!plan.encode_geometry_resample) {
    workspace.AliasImageFrom(plan.geometry_output, plan.sensor_linear_output);
    return;
  }
  auto& dest = AcquireRgba(workspace, plan.geometry_output, plan.geometry.render_extent.width,
                           plan.geometry.render_extent.height);
  sensor     = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr) {
    throw std::runtime_error("ExecuteOpenClGeometryResample: sensor texture lost during acquire");
  }

  const auto&            gpu = plan.geometry.gpu_data;
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
      .use_bicubic = plan.geometry.filter == TextureFilter::Bicubic ? 1U : 0U,
  };
  cl_mem src_mem = sensor->Texture().Native();
  cl_mem dst_mem = dest.Texture().Native();
  auto   kernel  = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kGeometryCameraProgramName, OpenCL::GpuDag::kGeometryResampleKernelName);
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src_mem), "geometry arg0");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst_mem), "geometry arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "geometry arg2");
  DispatchKernel(device, kernel, gpu.render_width, gpu.render_height);
}

void ExecuteOpenClCameraColor(OpenClRenderDevice& device, const ExecutionPlan& plan,
                              const PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClCameraColor: BeginRender has not been called");
  }
  const auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteOpenClCameraColor: missing develop node");
  }
  const auto resolved = ResolveDevelopColorTransform(develop->Params().Params());
  if (!resolved.ok) {
    throw std::runtime_error(std::string("ExecuteOpenClCameraColor: ") +
                             std::string(ColorTransformErrorMessage(resolved.error)));
  }
  auto* input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteOpenClCameraColor: missing geometry.scene_source");
  }
  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  auto&      output =
      workspace.AcquireImageForWrite(plan.develop_output, {width, height, TextureFormat::Rgba32f});
  input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr) {
    throw std::runtime_error("ExecuteOpenClCameraColor: geometry texture lost during acquire");
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
  const auto binding       = arena.Binding(key);
  cl_mem     src_mem       = input->Texture().Native();
  cl_mem     dst_mem       = output.Texture().Native();
  cl_mem     params_mem    = arena.DeviceBuffer().Native();
  const auto offset_floats = binding.offset / static_cast<std::uint32_t>(sizeof(float));
  auto       kernel        = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kGeometryCameraProgramName, OpenCL::GpuDag::kCameraColorKernelName);
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src_mem), "camera arg0");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst_mem), "camera arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), &params_mem), "camera arg2");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_uint), &offset_floats), "camera arg3");
  DispatchKernel(device, kernel, width, height);
}

}  // namespace alcedo

#endif
