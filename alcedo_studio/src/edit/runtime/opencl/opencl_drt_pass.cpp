//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_drt_pass.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_drt_gpu_params.hpp"
#include "edit/runtime/opencl/opencl_drt_params.hpp"
#include "edit/runtime/opencl/opencl_neighbor_dispatch.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

using OpenClToOutputParams            = OpenCL::Pipeline::OpenClToOutputParams;
constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

void DispatchDrt(OpenClRenderDevice& device, const OpenClBackend::Texture2D& source,
                 OpenClBackend::Texture2D& destination, const OpenClBackend::Buffer& parameters,
                 std::uint32_t parameter_offset) {
  if (parameters.Empty()) {
    throw std::runtime_error("ExecuteOpenClDrt: parameter buffer is empty");
  }
  auto          kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kDrtProgramName,
                                                                 OpenCL::GpuDag::kDrtKernelName);
  cl_mem        source_image      = source.Native();
  cl_mem        destination_image = destination.Native();
  cl_mem        parameter_buffer  = parameters.Native();
  const cl_uint offset            = parameter_offset;
  cl_int        error             = CL_SUCCESS;
  error |= clSetKernelArg(kernel, 0, sizeof(cl_mem), &source_image);
  error |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &destination_image);
  error |= clSetKernelArg(kernel, 2, sizeof(cl_mem), &parameter_buffer);
  error |= clSetKernelArg(kernel, 3, sizeof(cl_uint), &offset);
  CheckOpenCl(error, "ExecuteOpenClDrt: clSetKernelArg");

  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(source.Width()) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(source.Height()) + 15) / 16) * 16};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              "ExecuteOpenClDrt: clEnqueueNDRangeKernel");
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

void DispatchDrtScene(OpenClRenderDevice& device, const FrameSceneBinding& source,
                      const FrameSceneBinding& destination, const OpenClBackend::Buffer& parameters,
                      std::uint32_t parameter_offset, std::uint32_t width, std::uint32_t height) {
  if (parameters.Empty()) {
    throw std::runtime_error("ExecuteOpenClDrt: parameter buffer is empty");
  }
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kDrtProgramName,
                                                        OpenCL::GpuDag::kDrtSceneKernelName);
  auto& backend = device.Workspace().Device();
  BindOpenClSceneView(kernel, 0, OpenClBindScene(device, source), backend, "OpenCL DRT source",
                      OpenClSceneArgAccess::Read);
  BindOpenClSceneView(kernel, 3, OpenClBindScene(device, destination), backend,
                      "OpenCL DRT destination", OpenClSceneArgAccess::Write);
  cl_mem        parameter_buffer = parameters.Native();
  const cl_uint offset           = parameter_offset;
  CheckOpenCl(clSetKernelArg(kernel, 6, sizeof(cl_mem), &parameter_buffer),
              "ExecuteOpenClDrt: parameters");
  CheckOpenCl(clSetKernelArg(kernel, 7, sizeof(cl_uint), &offset), "ExecuteOpenClDrt: offset");
  CheckOpenCl(clSetKernelArg(kernel, 8, sizeof(width), &width), "ExecuteOpenClDrt: width");
  CheckOpenCl(clSetKernelArg(kernel, 9, sizeof(height), &height), "ExecuteOpenClDrt: height");
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              "ExecuteOpenClDrt: clEnqueueNDRangeKernel");
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

struct OpenClDrtOps {
  using Device            = OpenClRenderDevice;
  using Texture           = OpenClBackend::Texture2D;
  using HorizontalScratch = ResourceLease<OpenClBackend>;
  using LutBinding        = OpenClLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteOpenClDrt";

  /// Post neighborhood parameters travel in the command, not in an arena slot.
  static void RefreshNeighborhoodAdjustment(OpenClRenderDevice&, const IOperatorModel&,
                                            const ParameterSlotKey&, AdjustmentBehavior) {}

  static void PrepareNeighborCommands(OpenClRenderDevice&, const NodeId&,
                                      std::span<const std::uint32_t>) {}

  static auto NeighborLut(OpenClRenderDevice& device) -> LutBinding {
    return device.Workspace().Device().DummyLut();
  }

  static auto AcquireHorizontalScratch(OpenClRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch {
    return device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
  }

  static auto BindingWidth(OpenClRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return OpenClSceneWidth(device, binding);
  }

  static auto BindingHeight(OpenClRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return OpenClSceneHeight(device, binding);
  }

  static void DispatchHorizontal(OpenClRenderDevice& device, const FrameSceneBinding& src,
                                 HorizontalScratch& scratch, const NeighborWork& work,
                                 std::uint32_t width, std::uint32_t height) {
    EnqueueOpenClNeighborHorizontalScene(device, OpenClBindScene(device, src), scratch.Texture(),
                                         work.params, width, height);
  }

  static void DispatchVerticalApply(OpenClRenderDevice& device, const FrameSceneBinding& src,
                                    HorizontalScratch& scratch, const FrameSceneBinding& dst,
                                    const FrameSceneBinding& original, const LutBinding&,
                                    const NeighborWork& work, float mix, const GraphValueId* mask_id,
                                    std::uint32_t width, std::uint32_t height) {
    const auto src_view  = OpenClBindScene(device, src);
    const auto dst_view  = OpenClBindScene(device, dst);
    const auto orig_view = OpenClBindScene(device, original);
    EnqueueOpenClNeighborVerticalScene(device, src_view, scratch.Texture(), dst_view, &orig_view,
                                       mask_id, mix, work.params, width, height);
  }

  static void AcquireDisplayOutput(OpenClRenderDevice& device, const GraphValueId& id,
                                   std::uint32_t width, std::uint32_t height) {
    (void)device.Workspace().AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
  }

  /**
   * @brief Pack the display transform when the slot is missing, the DRT revision changed,
   *        or this frame overrides the output color.
   *
   * An override is recorded without a revision, so the next plain frame packs again.
   */
  static void BindDisplayParams(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                const DrtNodeModel& drt) {
    auto&                  arena = device.Workspace().Parameters();
    const ParameterSlotKey key{drt.Id(), AdjustmentInstanceId{"drt.output"}};
    const bool             overridden = plan.output_color_override.has_value();
    const auto             revision   = drt.Params().Revision();
    if (!overridden && arena.Contains(key) && arena.AppliedRevision(key) == revision) {
      return;
    }
    const auto runtime = PackOpenClDrtParams(
        DrtOutputResolver::ResolveNode(drt, plan.output_color_override, kErrorPrefix));
    arena.BindOrWritePackedSlot(key, DirtyFieldMask{kDrtDirtyBits}, runtime,
                                overridden ? kNoParameterRevision : revision);
  }

  static void DispatchDisplayTransform(OpenClRenderDevice& device, const FrameSceneBinding& scene,
                                       const FrameSceneBinding& display, const NodeId& drt_id,
                                       std::uint32_t width, std::uint32_t height) {
    auto&                  arena   = device.Workspace().Parameters();
    const ParameterSlotKey key{drt_id, AdjustmentInstanceId{"drt.output"}};
    const auto&            binding = arena.Binding(key);
    DispatchDrtScene(device, scene, display, arena.DeviceBuffer(), binding.offset, width, height);
  }

  static void CheckAfterEncode(OpenClRenderDevice&) {}
};

}  // namespace

auto ExecuteOpenClDiffusionFilter(OpenClRenderDevice& device, const PipelineDocument& document,
                                  const FrameSceneBinding& scene) -> FrameSceneBinding {
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    throw std::runtime_error("ExecuteOpenClDiffusionFilter: missing DRT endpoint");
  }
  if (IsDiffusionFilterActive(drt->Params().DiffusionStrength())) {
    throw std::runtime_error("OpenCL DiffusionFilter scatter is not implemented");
  }
  const auto output = FrameSceneBinding::WorkImage(DestinationWorkMember(scene));
  const auto width  = OpenClSceneWidth(device, scene);
  const auto height = OpenClSceneHeight(device, scene);
  if (OpenClSceneWidth(device, output) != width || OpenClSceneHeight(device, output) != height) {
    throw std::runtime_error(
        "ExecuteOpenClDiffusionFilter: scene-work extent does not match scene");
  }
  auto  kernel  = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kDrtProgramName, OpenCL::GpuDag::kDiffusionFilterDecodeSceneKernelName);
  auto& backend = device.Workspace().Device();
  BindOpenClSceneView(kernel, 0, OpenClBindScene(device, scene), backend,
                      "OpenCL DiffusionFilter source", OpenClSceneArgAccess::Read);
  BindOpenClSceneView(kernel, 3, OpenClBindScene(device, output), backend,
                      "OpenCL DiffusionFilter destination", OpenClSceneArgAccess::Write);
  CheckOpenCl(clSetKernelArg(kernel, 6, sizeof(width), &width),
              "ExecuteOpenClDiffusionFilter: width");
  CheckOpenCl(clSetKernelArg(kernel, 7, sizeof(height), &height),
              "ExecuteOpenClDiffusionFilter: height");
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(backend.NativeQueue(), kernel, 2, nullptr, global, local, 0,
                                     nullptr, &event),
              "ExecuteOpenClDiffusionFilter: clEnqueueNDRangeKernel");
  NoteOpenClEnqueueNdRange();
  backend.TrackKernelEvent(device.CommandContext(), event);
  return output;
}

auto ExecuteOpenClDrt(OpenClRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document, const FrameSceneBinding& scene)
    -> OpenClDrtResult {
  const auto executed = DrtPostExecutor<OpenClDrtOps>::Execute(device, plan, document, scene);
  return {executed.output, executed.display_post, executed.post_neighborhood_count};
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
