//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_drt_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/GPU_kernels/opencl_param.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_drt_params.hpp"
#include "edit/runtime/drt_display.hpp"
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

auto AcquireRgba(OpenClRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<OpenClBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto AcquireScratch(OpenClRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height)
    -> ResourceLease<OpenClBackend> {
  return workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
}

auto NeighborVerticalRadius(const GradeNeighborParams& params) -> std::uint32_t {
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  if (behavior == AdjustmentBehavior::Halation) {
    return std::clamp(static_cast<std::uint32_t>(std::ceil(params.sigma_y * 3.0f)), 1U,
                      kGradeNeighborMaxTapCount - 1U);
  }
  return params.radius;
}

void DispatchKernel(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t width,
                    std::uint32_t height, std::size_t local_edge = 16) {
  const std::size_t local[2]  = {local_edge, local_edge};
  const std::size_t global[2] = {
      ((static_cast<std::size_t>(width) + local_edge - 1) / local_edge) * local_edge,
      ((static_cast<std::size_t>(height) + local_edge - 1) / local_edge) * local_edge};
  cl_event event = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              "OpenCL DRT enqueue");
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

void DispatchNeighbor(OpenClRenderDevice& device, const OpenClBackend::Texture2D& src,
                      OpenClBackend::Texture2D& blur_horizontal, OpenClBackend::Texture2D& dst,
                      const GradeNeighborParams& params, std::uint32_t width,
                      std::uint32_t height) {
  auto blur_kernel =
      OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kPrimaryGradeProgramName,
                                              OpenCL::GpuDag::kPrimaryGradeNeighborBlurKernelName);
  auto apply_kernel =
      OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kPrimaryGradeProgramName,
                                              OpenCL::GpuDag::kPrimaryGradeNeighborApplyKernelName);

  const auto src_mem  = src.Native();
  const auto blur_mem = blur_horizontal.Native();
  const auto dst_mem  = dst.Native();
  CheckOpenCl(clSetKernelArg(blur_kernel, 0, sizeof(cl_mem), &src_mem),
              "OpenCL DRT neighbor source argument");
  CheckOpenCl(clSetKernelArg(blur_kernel, 1, sizeof(cl_mem), &blur_mem),
              "OpenCL DRT neighbor blur argument");
  CheckOpenCl(clSetKernelArg(blur_kernel, 2, sizeof(params), &params),
              "OpenCL DRT neighbor parameters");
  constexpr std::size_t kLocalEdge = 8;
  DispatchKernel(device, blur_kernel, width, height, kLocalEdge);

  CheckOpenCl(clSetKernelArg(apply_kernel, 0, sizeof(cl_mem), &src_mem),
              "OpenCL DRT neighbor apply source argument");
  CheckOpenCl(clSetKernelArg(apply_kernel, 1, sizeof(cl_mem), &blur_mem),
              "OpenCL DRT neighbor apply blur argument");
  CheckOpenCl(clSetKernelArg(apply_kernel, 2, sizeof(cl_mem), &dst_mem),
              "OpenCL DRT neighbor apply destination argument");
  CheckOpenCl(clSetKernelArg(apply_kernel, 3, sizeof(params), &params),
              "OpenCL DRT neighbor apply parameters");
  const auto radius      = NeighborVerticalRadius(params);
  const auto local_bytes = kLocalEdge * (kLocalEdge + 2U * radius) * 4U * sizeof(float);
  CheckOpenCl(clSetKernelArg(apply_kernel, 4, local_bytes, nullptr),
              "OpenCL DRT neighbor local tile argument");
  DispatchKernel(device, apply_kernel, width, height, kLocalEdge);
}

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

}  // namespace

auto ExecuteOpenClDrt(OpenClRenderDevice& device, const ExecutionPlan& plan,
                      PipelineDocument& document) -> OpenClDrtResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClDrt: BeginRender has not been called");
  }
  auto* drt = document.Drt();
  if (drt == nullptr) {
    throw std::runtime_error("ExecuteOpenClDrt: missing DRT endpoint");
  }
  auto* input = workspace.Images().Find(plan.SceneInputForDrt());
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteOpenClDrt: missing DRT scene input");
  }

  const auto width   = input->Texture().Width();
  const auto height  = input->Texture().Height();
  auto&      context = device.CommandContext();

  std::vector<PendingParameterPatch> post_pending;
  std::vector<GradeNeighborParams>   enabled;
  enabled.reserve(plan.drt.post_adjustments.size());
  for (const auto& compiled : plan.drt.post_adjustments) {
    auto* model = drt->FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      throw std::runtime_error("ExecuteOpenClDrt: compiled DRT/Post adjustment no longer matches");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value() || !IsNeighborhoodBehavior(*behavior)) {
      throw std::runtime_error(
          "ExecuteOpenClDrt: DRT/Post adjustment is not a neighborhood operation");
    }
    if (auto change = TakePendingParameterPatch(*model)) {
      post_pending.push_back(std::move(*change));
    }
    auto neighbor = MakeGradeNeighborParams(*model, *behavior, plan.geometry);
    if (neighbor.enabled != 0U) {
      enabled.push_back(neighbor);
    }
  }
  for (auto& patch : post_pending) {
    patch.Commit();
  }

  auto Resolve = [&](const GraphValueId& id) -> OpenClBackend::Texture2D& {
    auto* image = workspace.Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteOpenClDrt: scene image is missing");
    }
    return image->Texture();
  };

  GraphValueId scene_id = plan.SceneInputForDrt();
  if (enabled.empty()) {
    AcquireRgba(workspace, plan.drt.scene_output, width, height);
    input = workspace.Images().Find(plan.SceneInputForDrt());
    if (input == nullptr) {
      throw std::runtime_error("ExecuteOpenClDrt: DRT scene input lost during scene copy");
    }
    workspace.Device().CopyTexture2D(input->Texture(), Resolve(plan.drt.scene_output), context);
    scene_id = plan.drt.scene_output;
  } else {
    const GraphValueId ping_id{drt->Id(), PortId{"runtime.ping"}};
    const GraphValueId pong_id{drt->Id(), PortId{"runtime.pong"}};
    std::size_t        remaining = enabled.size();
    for (const auto& neighbor : enabled) {
      if (remaining == 0) {
        throw std::runtime_error("ExecuteOpenClDrt: neighborhood destination underflow");
      }
      --remaining;
      GraphValueId dest_id = plan.drt.scene_output;
      if (remaining != 0) {
        dest_id = scene_id == ping_id ? pong_id : ping_id;
      }
      AcquireRgba(workspace, dest_id, width, height);
      auto  blur_horizontal = AcquireScratch(workspace, width, height);
      auto& src             = Resolve(scene_id);
      auto& dest            = Resolve(dest_id);
      DispatchNeighbor(device, src, blur_horizontal.Texture(), dest, neighbor, width, height);
      scene_id = dest_id;
    }
  }

  auto&                       arena = workspace.Parameters();
  const ParameterSlotKey      key{drt->Id(), AdjustmentInstanceId{"drt.output"}};
  const ParameterFieldBinding field{DirtyFieldMask{kDrtDirtyBits}, 0, 0,
                                    sizeof(OpenClToOutputParams)};
  auto                        pending          = plan.output_color_override.has_value()
                                ? decltype(TakePendingParameterPatch(drt->Params())){}
                                : TakePendingParameterPatch(drt->Params());
  const bool                  needs_initialize = !arena.Contains(key);
  if (needs_initialize || pending.has_value() || plan.output_color_override.has_value()) {
    auto drt_json = drt->Params().ToJson();
    if (plan.output_color_override.has_value()) {
      OverlayExportColorOnDrtJson(drt_json, *plan.output_color_override);
    }
    const auto runtime = ResolveOpenClDrtParams(drt_json);
    auto       payload = std::make_shared<TypedOperatorParamPayload<OpenClToOutputParams>>(
        drt->Params().Type(), 1, runtime);
    if (needs_initialize) {
      arena.BindSlot(key, sizeof(OpenClToOutputParams), std::span{&field, 1});
      arena.InitializeFromFullDto(key, OperatorParamDto{drt->Params().Type(), 1, payload});
    } else {
      arena.ApplyPatch(
          key, OperatorParamPatchDto{drt->Id(), AdjustmentInstanceId{"drt.output"},
                                     drt->Params().Type(), DirtyFieldMask{kDrtDirtyBits}, payload});
    }
  }
  arena.UploadDirty(context);
  if (pending) {
    pending->Commit();
  }

  AcquireRgba(workspace, plan.display_output, width, height);
  auto* scene  = workspace.Images().Find(scene_id);
  auto* output = workspace.Images().Find(plan.display_output);
  if (scene == nullptr || output == nullptr) {
    throw std::runtime_error("ExecuteOpenClDrt: image cache changed during allocation");
  }
  const auto& binding = arena.Binding(key);
  DispatchDrt(device, scene->Texture(), output->Texture(), arena.DeviceBuffer(), binding.offset);
  return {plan.display_output, plan.drt.scene_output, static_cast<std::uint32_t>(enabled.size())};
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
