//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>

#include "cuda/cuda_check.hpp"
#include "cuda_drt_runtime_state.cuh"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/GPU_kernels/color_mgmt/disp_enc_funcs.cuh"
#include "edit/operators/GPU_kernels/color_mgmt/odt_funcs.cuh"
#include "edit/operators/GPU_kernels/color_mgmt/open_drt_funcs.cuh"
#include "edit/operators/cst/odt_op.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

auto EnsureDisplayImage(CudaRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                        std::uint32_t height) -> ResourceLease<CudaBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

void ResolveRuntime(CudaDrtRuntimeState& state, const DrtParamsModel& model) {
  ODT_Op descriptor(nlohmann::json{{"odt", model.ToJson()}});
  descriptor.SetGlobalParams(state.cpu_params);
  state.gpu_params = GPUParamsConverter::ConvertFromCPU(state.cpu_params, state.gpu_params);
}

__global__ void DrtKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                          const GPU_TO_OUTPUT_Params* params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  auto         runtime = *params;
  const float4 source  = input[index];
  const float3 scene   = make_float3(source.x, source.y, source.z);
  float3       display_linear;
  if (runtime.method_ == GPU_ODTMethod::ACES_2_0) {
    auto aces      = runtime.aces_params_;
    display_linear = CUDA::OutputTransform_fwd(scene, aces);
  } else {
    display_linear = CUDA::OpenDRTTransform_fwd(scene, runtime.open_drt_params_);
  }
  const float3 encoded = CUDA::DisplayEncoding(display_linear, runtime.limit_to_display_matx,
                                               runtime.eotf, runtime.display_linear_scale_);
  output[index]        = make_float4(encoded.x, encoded.y, encoded.z, source.w);
}

}  // namespace

CudaRenderDevice::CudaRenderDevice() : drt_runtime_(std::make_unique<CudaDrtRuntimeState>()) {}

CudaRenderDevice::~CudaRenderDevice() {
  try {
    WaitIdle();
  } catch (...) {
  }
}

void CudaRenderDevice::CancelRender() noexcept {
  if (!workspace_.IsRendering()) return;
  (void)::cudaStreamSynchronize(command_context_.Stream());
  workspace_.CancelRender();
}

auto CudaRenderDevice::DrtRuntime() -> CudaDrtRuntimeState& { return *drt_runtime_; }

auto ExecuteCudaDrt(CudaRenderDevice& device, const ExecutionPlan& plan, PipelineDocument& document)
    -> CudaDrtResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaDrt: BeginRender has not been called");
  }
  auto* drt = document.Drt();
  if (drt == nullptr) throw std::runtime_error("ExecuteCudaDrt: missing DRT endpoint");
  auto* input = workspace.Images().Find(plan.primary_grade_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteCudaDrt: missing primary-grade output");
  }

  auto&                       arena = workspace.Parameters();
  const ParameterSlotKey      key{drt->Id(), AdjustmentInstanceId{"drt.output"}};
  const ParameterFieldBinding field{DirtyFieldMask{kDrtDirtyBits}, 0, 0,
                                    sizeof(GPU_TO_OUTPUT_Params)};
  auto                        pending          = TakePendingParameterPatch(drt->Params());
  const bool                  needs_initialize = !arena.Contains(key);
  if (needs_initialize || pending.has_value()) {
    ResolveRuntime(device.DrtRuntime(), drt->Params());
    const auto runtime = device.DrtRuntime().gpu_params.to_output_params_;
    auto       payload = std::make_shared<TypedOperatorParamPayload<GPU_TO_OUTPUT_Params>>(
        drt->Params().Type(), 1, runtime);
    if (needs_initialize) {
      arena.BindSlot(key, sizeof(GPU_TO_OUTPUT_Params), std::span{&field, 1});
      arena.InitializeFromFullDto(key, OperatorParamDto{drt->Params().Type(), 1, payload});
    } else {
      arena.ApplyPatch(
          key, OperatorParamPatchDto{drt->Id(), AdjustmentInstanceId{"drt.output"},
                                     drt->Params().Type(), DirtyFieldMask{kDrtDirtyBits}, payload});
    }
  }
  arena.UploadDirty(device.CommandContext());
  if (pending) pending->Commit();

  const auto input_width  = input->Texture().Width();
  const auto input_height = input->Texture().Height();
  EnsureDisplayImage(workspace, plan.display_output, input_width, input_height);
  input        = workspace.Images().Find(plan.primary_grade_output);
  auto* output = workspace.Images().Find(plan.display_output);
  if (input == nullptr || output == nullptr) {
    throw std::runtime_error("ExecuteCudaDrt: image cache changed during allocation");
  }
  const auto& binding = arena.Binding(key);
  const auto* params  = reinterpret_cast<const GPU_TO_OUTPUT_Params*>(
      static_cast<const std::byte*>(arena.DeviceBuffer().DevicePointer()) + binding.offset);
  const std::uint32_t     pixels = input_width * input_height;
  constexpr std::uint32_t block  = 256;
  DrtKernel<<<(pixels + block - 1) / block, block, 0, device.CommandContext().Stream()>>>(
      static_cast<const float4*>(input->Texture().DevicePointer()),
      static_cast<float4*>(output->Texture().DevicePointer()), pixels, params);
  cuda::CheckCuda(::cudaGetLastError(), "ExecuteCudaDrt: kernel launch");
  return {plan.display_output};
}

}  // namespace alcedo
