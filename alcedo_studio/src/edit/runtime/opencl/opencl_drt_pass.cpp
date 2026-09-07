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
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/GPU_kernels/opencl_param.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_drt_params.hpp"
#include "edit/runtime/opencl/opencl_neighbor_dispatch.hpp"
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

struct OpenClDrtOps {
  using Device            = OpenClRenderDevice;
  using Texture           = OpenClBackend::Texture2D;
  using HorizontalScratch = ResourceLease<OpenClBackend>;
  using LutBinding        = OpenClLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteOpenClDrt";

  static auto RefreshNeighborhoodAdjustment(OpenClRenderDevice&, IOperatorModel& model,
                                            const ParameterSlotKey&, AdjustmentBehavior)
      -> std::optional<PendingParameterPatch> {
    return TakePendingDirtyFields(model);
  }

  static void PrepareNeighborCommands(OpenClRenderDevice&, const NodeId&,
                                      std::span<const std::uint32_t>) {}

  static auto NeighborLut(OpenClRenderDevice& device) -> LutBinding {
    return device.Workspace().Device().DummyLut();
  }

  static auto AcquireHorizontalScratch(OpenClRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch {
    return device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
  }

  static auto HorizontalScratchTexture(HorizontalScratch& scratch) -> Texture& {
    return scratch.Texture();
  }

  static void DispatchHorizontal(OpenClRenderDevice& device, const Texture& src, Texture& blur,
                                 const NeighborWork& work, std::uint32_t width,
                                 std::uint32_t height) {
    EnqueueOpenClNeighborHorizontal(device, src, blur, work.params, width, height);
  }

  static void DispatchVerticalApply(OpenClRenderDevice& device, const Texture& src,
                                    const Texture& blur, Texture& dst, const LutBinding&,
                                    const NeighborWork& work, std::uint32_t width,
                                    std::uint32_t height) {
    EnqueueOpenClNeighborVertical(device, src, blur, dst, work.params, width, height);
  }

  static auto AcquireOutput(OpenClRenderDevice& device, const GraphValueId& id, std::uint32_t width,
                            std::uint32_t height) -> Texture& {
    return device.Workspace()
        .AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f})
        .Texture();
  }

  static auto SceneTexture(OpenClRenderDevice& device, const GraphValueId& id) -> Texture& {
    auto* image = device.Workspace().Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteOpenClDrt: scene image is missing");
    }
    return image->Texture();
  }

  static void CopyTexture(OpenClRenderDevice& device, const GraphValueId& src,
                          const GraphValueId& dst) {
    device.Workspace().Device().CopyTexture2D(SceneTexture(device, src), SceneTexture(device, dst),
                                              device.CommandContext());
  }

  static void BindDisplayParams(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                DrtNodeModel& drt, std::vector<PendingParameterPatch>& pending) {
    auto&                  arena = device.Workspace().Parameters();
    const ParameterSlotKey key{drt.Id(), AdjustmentInstanceId{"drt.output"}};
    auto                   display_pending = plan.output_color_override.has_value()
                                                 ? decltype(TakePendingDirtyFields(drt.Params())){}
                                                 : TakePendingDirtyFields(drt.Params());
    const bool             needs_initialize = !arena.Contains(key);
    if (needs_initialize || display_pending.has_value() || plan.output_color_override.has_value()) {
      auto drt_json = drt.Params().ToJson();
      if (plan.output_color_override.has_value()) {
        OverlayExportColorOnDrtJson(drt_json, *plan.output_color_override);
      }
      const auto runtime = ResolveOpenClDrtParams(drt_json);
      arena.BindOrWritePackedSlot(key, DirtyFieldMask{kDrtDirtyBits}, runtime);
    }
    if (display_pending) {
      pending.push_back(std::move(*display_pending));
    }
  }

  static void DispatchDisplayTransform(OpenClRenderDevice& device, const Texture& scene,
                                       Texture& display, const NodeId& drt_id, std::uint32_t,
                                       std::uint32_t) {
    auto&                  arena   = device.Workspace().Parameters();
    const ParameterSlotKey key{drt_id, AdjustmentInstanceId{"drt.output"}};
    const auto&            binding = arena.Binding(key);
    DispatchDrt(device, scene, display, arena.DeviceBuffer(), binding.offset);
  }

  static void CheckAfterEncode(OpenClRenderDevice&) {}
};

}  // namespace

auto ExecuteOpenClDrt(OpenClRenderDevice& device, const ExecutionPlan& plan,
                      PipelineDocument& document) -> OpenClDrtResult {
  const auto executed = DrtPostExecutor<OpenClDrtOps>::Execute(device, plan, document);
  return {executed.output, executed.display_post, executed.post_neighborhood_count};
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
