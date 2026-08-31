//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_drt_pass.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/GPU_kernels/opencl_param.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
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
  auto* input = workspace.Images().Find(plan.primary_grade_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteOpenClDrt: missing primary-grade output");
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
  arena.UploadDirty(device.CommandContext());
  if (pending) {
    pending->Commit();
  }

  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  workspace.AcquireImageForWrite(plan.display_output, {width, height, TextureFormat::Rgba32f});
  input        = workspace.Images().Find(plan.primary_grade_output);
  auto* output = workspace.Images().Find(plan.display_output);
  if (input == nullptr || output == nullptr) {
    throw std::runtime_error("ExecuteOpenClDrt: image cache changed during allocation");
  }
  const auto& binding = arena.Binding(key);
  DispatchDrt(device, input->Texture(), output->Texture(), arena.DeviceBuffer(), binding.offset);
  return {plan.display_output};
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
