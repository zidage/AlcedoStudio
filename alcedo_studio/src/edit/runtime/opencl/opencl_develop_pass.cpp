//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_develop_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "decoders/dng_default_crop.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_tiled.hpp"
#include "decoders/processor/operators/gpu/opencl_encode.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/camera_color_gpu_params.hpp"
#include "edit/runtime/develop_demosaic.hpp"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_neural_session_workspace.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/gpu_pool_trace.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "gpu/transient_buffer_scope.hpp"
#include "gpu/transient_last_use.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

OpenClDemosaicNetModelCache* g_opencl_neural_cache_for_test = nullptr;

void                         TraceDevelopStage(const char* stage) {
  if (GpuPoolTraceVerbose()) {
    std::fprintf(stderr, "[GPU_POOL] OpenCL Develop %s\n", stage);
  }
}

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

struct HighlightScratch {
  void*                    mask_ptr    = nullptr;
  void*                    dilated_ptr = nullptr;
  void*                    sums_ptr    = nullptr;
  void*                    cnts_ptr    = nullptr;
  void*                    clipped_ptr = nullptr;
  opencl::OpenClBufferView mask;
  opencl::OpenClBufferView dilated;
  opencl::OpenClBufferView sums;
  opencl::OpenClBufferView cnts;
  opencl::OpenClBufferView clipped;
};

struct LegacyDemosaicScratch {
  void*                           green_ptr    = nullptr;
  void*                           rgba_ptr     = nullptr;
  void*                           hlr_rgba_ptr = nullptr;
  void*                           r_ptr        = nullptr;
  void*                           g_ptr        = nullptr;
  void*                           b_ptr        = nullptr;
  void*                           vh_ptr       = nullptr;
  void*                           pq_ptr       = nullptr;
  opencl::OpenClBufferView        green;
  opencl::OpenClBufferView        rgba;
  opencl::OpenClBufferView        hlr_rgba;
  opencl::OpenClBufferView        r;
  opencl::OpenClBufferView        g;
  opencl::OpenClBufferView        b;
  opencl::OpenClBufferView        vh;
  opencl::OpenClBufferView        pq;
  std::optional<HighlightScratch> highlight;
};

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

void EncodeWarp(OpenClRenderDevice& device, cl_mem src, cl_mem dst,
                const dng::WarpRectilinear& warp, std::uint32_t width, std::uint32_t height) {
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

void FillHighlightScratch(OpenClRenderDevice& device, void* mask_ptr, std::size_t mask_bytes,
                          void* dilated_ptr, void* sums_ptr, void* cnts_ptr, void* clipped_ptr) {
  auto& backend = device.Workspace().Device();
  backend.FillDeviceMemory(mask_ptr, mask_bytes, 0, device.CommandContext());
  backend.FillDeviceMemory(dilated_ptr, mask_bytes, 0, device.CommandContext());
  backend.FillDeviceMemory(sums_ptr, 4 * sizeof(float), 0, device.CommandContext());
  backend.FillDeviceMemory(cnts_ptr, 4 * sizeof(float), 0, device.CommandContext());
  backend.FillDeviceMemory(clipped_ptr, sizeof(cl_int), 0, device.CommandContext());
}

auto AllocateHighlightScratch(OpenClRenderDevice& device, std::uint32_t width, std::uint32_t height)
    -> HighlightScratch {
  auto&             backend     = device.Workspace().Device();
  auto&             transients  = device.Workspace().TransientBuffers();
  const std::size_t mask_bytes  = static_cast<std::size_t>(width) * height * 6;
  void*             mask_ptr    = transients.Allocate(mask_bytes);
  void*             dilated_ptr = transients.Allocate(mask_bytes);
  void*             sums_ptr    = transients.Allocate(4 * sizeof(float));
  void*             cnts_ptr    = transients.Allocate(4 * sizeof(float));
  void*             clipped_ptr = transients.Allocate(sizeof(cl_int));
  FillHighlightScratch(device, mask_ptr, mask_bytes, dilated_ptr, sums_ptr, cnts_ptr, clipped_ptr);
  return HighlightScratch{
      .mask_ptr    = mask_ptr,
      .dilated_ptr = dilated_ptr,
      .sums_ptr    = sums_ptr,
      .cnts_ptr    = cnts_ptr,
      .clipped_ptr = clipped_ptr,
      .mask        = ViewFromPtr(backend, mask_ptr, mask_bytes),
      .dilated     = ViewFromPtr(backend, dilated_ptr, mask_bytes),
      .sums        = ViewFromPtr(backend, sums_ptr, 4 * sizeof(float)),
      .cnts        = ViewFromPtr(backend, cnts_ptr, 4 * sizeof(float)),
      .clipped     = ViewFromPtr(backend, clipped_ptr, sizeof(cl_int)),
  };
}

void CopyRgbaToPacked(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView src,
                      cl_mem packed, const PreparedRawInput& input, std::uint32_t width,
                      std::uint32_t height, bool apply_cam_mul) {
  const float identity[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  OpenCL::EncodeCopyRgbaCropInverseOrient(
      stream, src, packed, CropOrFull(input, width, height), width,
      apply_cam_mul ? input.linearization.cam_mul : identity, input.sensor.orientation_flip);
}

void EncodeHighlightFromRgbaAndPack(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView src,
                                    cl_mem packed, const PreparedRawInput& input,
                                    std::uint32_t src_width, std::uint32_t src_height,
                                    opencl::OpenClBufferView dst, const HighlightScratch& scratch) {
  OpenCL::EncodeHighlightReconstruct(stream, src, dst, scratch.mask, scratch.dilated, scratch.sums,
                                     scratch.cnts, scratch.clipped, input.linearization.cam_mul,
                                     src_width, src_height);
  CopyRgbaToPacked(stream, dst, packed, input, src_width, src_height, true);
}

void EncodePlanarHighlightAndPack(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView r,
                                  opencl::OpenClBufferView g, opencl::OpenClBufferView b,
                                  cl_mem packed, const PreparedRawInput& input,
                                  std::uint32_t plane_width, std::uint32_t plane_height,
                                  const HighlightScratch& scratch) {
  const auto crop = CropOrFull(input, plane_width, plane_height);
  OpenCL::EncodeHighlightReconstructPlanarAndPack(stream, r, g, b, packed, scratch.mask,
                                                  scratch.dilated, scratch.sums, scratch.cnts,
                                                  scratch.clipped, input.linearization.cam_mul,
                                                  crop, plane_width, input.sensor.orientation_flip);
}

auto AllocateLegacyDemosaicScratch(OpenClRenderDevice& device, const PreparedRawInput& input,
                                   bool hlr) -> LegacyDemosaicScratch {
  auto&      transients  = device.Workspace().TransientBuffers();
  auto&      backend     = device.Workspace().Device();
  const auto width       = input.host_extent.width;
  const auto height      = input.host_extent.height;
  const auto plane_bytes = static_cast<std::size_t>(width) * height * sizeof(float);
  const auto rgba_bytes  = plane_bytes * 4;

  LegacyDemosaicScratch scratch;
  if (input.cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    scratch.green_ptr = transients.Allocate(plane_bytes);
    scratch.rgba_ptr  = transients.Allocate(rgba_bytes);
    scratch.green     = ViewFromPtr(backend, scratch.green_ptr, plane_bytes);
    scratch.rgba      = ViewFromPtr(backend, scratch.rgba_ptr, rgba_bytes);
    if (hlr) {
      scratch.hlr_rgba_ptr = transients.Allocate(rgba_bytes);
      scratch.hlr_rgba     = ViewFromPtr(backend, scratch.hlr_rgba_ptr, rgba_bytes);
      scratch.highlight    = AllocateHighlightScratch(device, width, height);
    }
    return scratch;
  }

  scratch.r_ptr  = transients.Allocate(plane_bytes);
  scratch.g_ptr  = transients.Allocate(plane_bytes);
  scratch.b_ptr  = transients.Allocate(plane_bytes);
  scratch.vh_ptr = transients.Allocate(plane_bytes);
  scratch.pq_ptr = transients.Allocate(plane_bytes);
  scratch.r      = ViewFromPtr(backend, scratch.r_ptr, plane_bytes);
  scratch.g      = ViewFromPtr(backend, scratch.g_ptr, plane_bytes);
  scratch.b      = ViewFromPtr(backend, scratch.b_ptr, plane_bytes);
  scratch.vh     = ViewFromPtr(backend, scratch.vh_ptr, plane_bytes);
  scratch.pq     = ViewFromPtr(backend, scratch.pq_ptr, plane_bytes);
  if (hlr) {
    const auto crop   = CropOrFull(input, width, height);
    scratch.highlight = AllocateHighlightScratch(device, static_cast<std::uint32_t>(crop.width),
                                                 static_cast<std::uint32_t>(crop.height));
  }
  return scratch;
}

void ReleaseHighlightScratch(OpenClRenderDevice& device, const HighlightScratch& scratch) {
  ReleaseTransientSlabsAfterGpuLastUse(device, {scratch.mask_ptr, scratch.dilated_ptr,
                                               scratch.sums_ptr, scratch.cnts_ptr,
                                               scratch.clipped_ptr});
}

void EncodeLegacyDemosaic(OpenClRenderDevice& device, opencl::OpenClEncodeQueue& stream,
                          const PreparedRawInput& input, opencl::OpenClBufferView linear,
                          void* linear_ptr, cl_mem packed, bool hlr,
                          const LegacyDemosaicScratch& scratch) {
  const auto width  = input.host_extent.width;
  const auto height = input.host_extent.height;

  if (input.cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    OpenCL::EncodeXTrans(stream, linear, scratch.green, scratch.rgba,
                         input.cfa_pattern.xtrans_pattern, width, height,
                         input.downsample_passes == 0 ? 3 : 1);
    ReleaseTransientSlabsAfterGpuLastUse(device, {linear_ptr, scratch.green_ptr});
    if (hlr) {
      EncodeHighlightFromRgbaAndPack(stream, scratch.rgba, packed, input, width, height,
                                     scratch.hlr_rgba, *scratch.highlight);
      ReleaseHighlightScratch(device, *scratch.highlight);
      ReleaseTransientSlabsAfterGpuLastUse(device, {scratch.rgba_ptr, scratch.hlr_rgba_ptr});
      return;
    }
    CopyRgbaToPacked(stream, scratch.rgba, packed, input, width, height, true);
    ReleaseTransientSlabsAfterGpuLastUse(device, {scratch.rgba_ptr});
    return;
  }

  OpenCL::EncodeBayerRcd(stream, linear, scratch.r, scratch.g, scratch.b, scratch.vh, scratch.pq,
                         input.cfa_pattern.bayer_pattern, width, height);
  ReleaseTransientSlabsAfterGpuLastUse(device, {linear_ptr, scratch.vh_ptr, scratch.pq_ptr});
  if (hlr) {
    EncodePlanarHighlightAndPack(stream, scratch.r, scratch.g, scratch.b, packed, input, width,
                                 height, *scratch.highlight);
    ReleaseHighlightScratch(device, *scratch.highlight);
    ReleaseTransientSlabsAfterGpuLastUse(device, {scratch.r_ptr, scratch.g_ptr, scratch.b_ptr});
    return;
  }
  OpenCL::EncodePackPlanesCropInverseOrient(
      stream, scratch.r, scratch.g, scratch.b, packed, CropOrFull(input, width, height), width,
      input.linearization.cam_mul, input.sensor.orientation_flip);
  ReleaseTransientSlabsAfterGpuLastUse(device, {scratch.r_ptr, scratch.g_ptr, scratch.b_ptr});
}

void EncodeNeural(OpenClRenderDevice& device, opencl::OpenClEncodeQueue& stream,
                  const PreparedRawInput& input, opencl::OpenClBufferView linear, void* linear_ptr,
                  cl_mem packed, bool hlr) {
  std::string error;
  const int   min_spatial = input.cfa_pattern.kind == RawCfaKind::XTrans6x6
                                ? DemosaicNetXTransSpec::kMinSpatial
                                : DemosaicNetBayerSpec::kMinSpatial;
  const auto  geometry =
      ComputeNeuralAlignedGeometry(input.cfa_pattern, static_cast<int>(input.host_extent.width),
                                   static_cast<int>(input.host_extent.height), min_spatial, &error);
  if (!geometry.has_value()) {
    throw std::runtime_error("ExecuteOpenClDevelop: Neural Engine preprocess failed: " + error);
  }

  OpenClDemosaicNetLoadOptions load_options;
  if (g_opencl_neural_cache_for_test != nullptr) {
    load_options.model_dir = "alcedo-missing-demosaicnet-models";
  }
  OpenClDemosaicNetModelCache& cache    = g_opencl_neural_cache_for_test == nullptr
                                              ? OpenClDemosaicNetModelCache::Instance()
                                              : *g_opencl_neural_cache_for_test;
  const bool                   is_bayer = input.cfa_pattern.kind == RawCfaKind::Bayer2x2;
  const auto                   variant =
      is_bayer ? OpenClDemosaicNetVariant::Bayer : OpenClDemosaicNetVariant::XTrans;
  load_options.queue = device.Workspace().Device().NativeQueue();
  if (!cache.EnsureLoaded(variant, load_options)) {
    throw std::runtime_error(std::string("ExecuteOpenClDevelop: Neural Engine unavailable: ") +
                             cache.LastError());
  }

  std::lock_guard<std::mutex> neural_decode(OpenClNeuralDecodeMutex());
  auto&                       backend = device.Workspace().Device();
  auto&                       neural  = backend.NeuralDemosaicWorkspace();
  const auto width   = input.host_extent.width;
  const auto height  = input.host_extent.height;
  if (linear.offset_bytes % sizeof(float) != 0) {
    throw std::runtime_error("ExecuteOpenClDevelop: Neural CFA offset is not float-aligned");
  }

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

  // Assemble tiles into RGBA. A second full-frame HWC3 plane plus HLR RGBA
  // exceeds OpenCL MaxTransientBytes on 100MP (CL_DEVICE_MAX_MEM_ALLOC_SIZE
  // packing), so the DAG path does not keep a 3-channel canvas.
  const std::size_t rgba_bytes = static_cast<std::size_t>(geometry->aligned_width) *
                                 geometry->aligned_height * 4 * sizeof(float);
  void* rgba_ptr = device.Workspace().TransientBuffers().Allocate(rgba_bytes);
  auto  rgba     = ViewFromPtr(backend, rgba_ptr, rgba_bytes);
  if (rgba.offset_bytes % sizeof(float) != 0) {
    throw std::runtime_error("ExecuteOpenClDevelop: Neural RGBA offset is not float-aligned");
  }

  OpenClDemosaicNetTiledDispatch dispatch;
  dispatch.input_mono_cfa       = linear.native;
  dispatch.src_width            = static_cast<int>(width);
  dispatch.src_height           = static_cast<int>(height);
  dispatch.crop_x               = geometry->shift_sx;
  dispatch.crop_y               = geometry->shift_sy;
  dispatch.mono_offset_floats   = static_cast<int>(linear.offset_bytes / sizeof(float));
  dispatch.output_aligned_hwc   = rgba.native;
  dispatch.output_offset_floats = static_cast<int>(rgba.offset_bytes / sizeof(float));
  dispatch.output_channels      = 4;
  dispatch.aligned_width        = geometry->aligned_width;
  dispatch.aligned_height       = geometry->aligned_height;
  dispatch.rgb_fc               = neural.cfa_table.get();
  dispatch.period               = period;
  dispatch.queue                = backend.NativeQueue();
  if (is_bayer) {
    (void)neural.executor.EnqueueBayer(cache.Bayer(), neural.slots, dispatch);
  } else {
    (void)neural.executor.EnqueueXTrans(cache.XTrans(), neural.slots, dispatch);
  }

  const auto aligned_w = static_cast<std::uint32_t>(geometry->aligned_width);
  const auto aligned_h = static_cast<std::uint32_t>(geometry->aligned_height);
  void*      hlr_ptr   = nullptr;
  std::optional<HighlightScratch> highlight;
  if (hlr) {
    hlr_ptr   = device.Workspace().TransientBuffers().Allocate(rgba_bytes);
    auto hlr_dst = ViewFromPtr(backend, hlr_ptr, rgba_bytes);
    highlight = AllocateHighlightScratch(device, aligned_w, aligned_h);
    EncodeHighlightFromRgbaAndPack(stream, rgba, packed, input, aligned_w, aligned_h, hlr_dst,
                                   *highlight);
  } else {
    OpenCL::EncodeCopyRgbaCropInverseOrient(
        stream, rgba, packed, CropOrFull(input, aligned_w, aligned_h), aligned_w,
        input.linearization.cam_mul, input.sensor.orientation_flip);
  }
  // Cached module kernels stay bound until this wait. Releasing the decode lock
  // before the queue drains lets another device Reset those cl_kernel arguments.
  backend.SynchronizeRecordedWork(device.CommandContext());
  auto& arena = device.Workspace().TransientBuffers();
  arena.ReleaseSlabContaining(linear_ptr);
  arena.ReleaseSlabContaining(rgba_ptr);
  arena.ReleaseSlabContaining(hlr_ptr);
  if (highlight.has_value()) {
    arena.ReleaseSlabContaining(highlight->mask_ptr);
    arena.ReleaseSlabContaining(highlight->dilated_ptr);
    arena.ReleaseSlabContaining(highlight->sums_ptr);
    arena.ReleaseSlabContaining(highlight->cnts_ptr);
    arena.ReleaseSlabContaining(highlight->clipped_ptr);
  }
}

}  // namespace

void SetOpenClDevelopNeuralModelCacheForTesting(OpenClDemosaicNetModelCache* cache) {
  g_opencl_neural_cache_for_test = cache;
}

void ExecuteOpenClDevelop(OpenClRenderDevice& device, const ExecutionPlan& plan,
                          const PreparedRawInput& input, PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClDevelop: BeginRender has not been called");
  }
  TransientAllocationPolicyScope<OpenClBackend> exact_release(
      workspace.TransientBuffers(), TransientAllocationPolicy::ExactRelease);
  auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteOpenClDevelop: missing develop node");
  }
  auto pending = TakePendingDirtyFields(develop->Params());
  const auto         flags       = develop->Params().Params();
  const bool         hlr         = flags.highlights_reconstruct;
  const auto         out_w       = plan.source.develop_output_extent.width;
  const auto         out_h       = plan.source.develop_output_extent.height;
  const GraphValueId sensor_id   = plan.sensor_linear_output;
  const GraphValueId demosaic_id = input.dng_warp_rectilinear.has_value()
                                       ? GraphValueId{NodeId{"develop"}, PortId{"sensor_unwarped"}}
                                       : sensor_id;
  if (input.input_kind == RawInputKind::DebayeredRgb ||
      plan.source.kind == DevelopInputKind::DirectRgb) {
    const auto width  = input.host_extent.width;
    const auto height = input.host_extent.height;
    if (input.pixels.format != HostPixelFormat::F32Rgba ||
        input.pixels.stride_bytes != width * 4 * sizeof(float)) {
      throw std::runtime_error("ExecuteOpenClDevelop: expected tightly packed RGB input");
    }
    auto&      backend  = workspace.Device();
    const auto bytes    = input.pixels.ByteCount();
    void*      uploaded = workspace.TransientBuffers().Allocate(bytes);
    const auto source   = ViewFromPtr(backend, uploaded, bytes);
    backend.UploadDeviceMemory(uploaded, input.pixels.Span(), device.CommandContext());
    auto stream = MakeEncodeQueue(device);
    OpenCL::EncodeLinearizeRgb(stream, source, width, height,
                               input.rgb_linearization.value_or(RawRgbLinearizationParams{}));
    auto& decoded = AcquireRgba(workspace, demosaic_id, out_w, out_h);
    if (hlr && input.rgb_linearization.has_value()) {
      void*      dst_ptr = workspace.TransientBuffers().Allocate(bytes);
      auto       dst     = ViewFromPtr(backend, dst_ptr, bytes);
      const auto scratch = AllocateHighlightScratch(device, width, height);
      EncodeHighlightFromRgbaAndPack(stream, source, decoded.Texture().Native(), input, width,
                                     height, dst, scratch);
      ReleaseHighlightScratch(device, scratch);
      ReleaseTransientSlabsAfterGpuLastUse(device, {uploaded, dst_ptr});
    } else {
      CopyRgbaToPacked(stream, source, decoded.Texture().Native(), input, width, height,
                       input.rgb_linearization.has_value());
      ReleaseTransientSlabsAfterGpuLastUse(device, {uploaded});
    }
  } else {
    const auto width  = input.host_extent.width;
    const auto height = input.host_extent.height;
    if (width == 0 || height == 0) {
      throw std::runtime_error("ExecuteOpenClDevelop: empty CFA");
    }
    if (input.pixels.stride_bytes != width * sizeof(std::uint16_t)) {
      throw std::runtime_error("ExecuteOpenClDevelop: CFA plane must be tightly packed");
    }

    auto&      backend    = workspace.Device();
    auto&      transients = workspace.TransientBuffers();
    const auto u16_bytes  = static_cast<std::size_t>(width) * height * sizeof(std::uint16_t);
    const auto f32_bytes  = static_cast<std::size_t>(width) * height * sizeof(float);
    void*      u16_ptr    = transients.Allocate(u16_bytes);
    TraceDevelopStage("allocate CFA U16 end");
    void* f32_ptr = transients.Allocate(f32_bytes);
    TraceDevelopStage("allocate linear F32 end");
    TraceDevelopStage("acquire output begin");
    auto& decoded_lease = AcquireRgba(workspace, demosaic_id, out_w, out_h);
    TraceDevelopStage("acquire output end");
    backend.UploadDeviceMemory(u16_ptr, input.pixels.Span(), device.CommandContext());
    TraceDevelopStage("enqueue CFA upload end");
    auto stream = MakeEncodeQueue(device);
    auto linear = ViewFromPtr(backend, f32_ptr, f32_bytes);
    OpenCL::EncodeToLinearRef(stream, ViewFromPtr(backend, u16_ptr, u16_bytes), linear, width,
                              height, input.linearization, input.cfa_pattern);
    TraceDevelopStage("enqueue linearize end");
    if (!hlr) {
      OpenCL::EncodeCfaClamp01(stream, linear, width, height);
      TraceDevelopStage("enqueue CFA clamp end");
    }
    ReleaseTransientSlabsAfterGpuLastUse(device, {u16_ptr});

    const auto method =
        ResolveDevelopDemosaicMethod(flags, input.cfa_pattern.kind, input.downsample_passes);
    if (method == RawDemosaicMethod::NeuralEngine) {
      TraceDevelopStage("neural begin");
      EncodeNeural(device, stream, input, linear, f32_ptr, decoded_lease.Texture().Native(), hlr);
      TraceDevelopStage("neural end");
    } else {
      TraceDevelopStage("allocate legacy scratch end");
      auto legacy_scratch = AllocateLegacyDemosaicScratch(device, input, hlr);
      TraceDevelopStage("legacy demosaic begin");
      EncodeLegacyDemosaic(device, stream, input, linear, f32_ptr, decoded_lease.Texture().Native(),
                           hlr, legacy_scratch);
      TraceDevelopStage("legacy demosaic end");
    }
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
                              PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClCameraColor: BeginRender has not been called");
  }
  auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteOpenClCameraColor: missing develop node");
  }
  auto pending = TakePendingDirtyFields(develop->Params());
  const auto develop_params = develop->Params().Params();
  const auto resolved       = ResolveDevelopColorTransform(develop_params);
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
  auto&                  arena = workspace.Parameters();
  const ParameterSlotKey key{develop->Id(), kDevelopCameraColorSlot};
  arena.BindOrWritePackedSlot(key, DirtyFieldMask{DevelopDirty::WhiteBalance}, gpu_params);
  arena.UploadDirty(device.CommandContext());
  const auto binding       = arena.Binding(key);
  cl_mem     src_mem       = input->Texture().Native();
  cl_mem     dst_mem       = output.Texture().Native();
  cl_mem     params_mem    = arena.DeviceBuffer().Native();
  const auto offset_floats = binding.offset / static_cast<std::uint32_t>(sizeof(float));
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kGeometryCameraProgramName,
                                                        OpenCL::GpuDag::kCameraColorKernelName);
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src_mem), "camera arg0");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst_mem), "camera arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), &params_mem), "camera arg2");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_uint), &offset_floats), "camera arg3");
  const auto table_data = PackDngProfileGpuData(develop_params.camera_profile, resolved.transform);
  auto&      tables =
      UploadDngProfileGpuData(workspace, develop->Id(), table_data, device.CommandContext());
  cl_mem table_mem = tables.Native();
  CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(cl_mem), &table_mem), "camera profile arg4");
  DispatchKernel(device, kernel, width, height);
  if (pending.has_value()) {
    pending->Commit();
  }
}

}  // namespace alcedo

#endif
