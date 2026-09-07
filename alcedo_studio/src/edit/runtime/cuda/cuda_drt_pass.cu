//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "cuda/cuda_check.hpp"
#include "cuda_acescc.cuh"
#include "cuda_drt_runtime_state.cuh"
#include "cuda_neighbor_grade.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/GPU_kernels/color_mgmt/disp_enc_funcs.cuh"
#include "edit/operators/GPU_kernels/color_mgmt/odt_funcs.cuh"
#include "edit/operators/GPU_kernels/color_mgmt/open_drt_funcs.cuh"
#include "edit/operators/cst/odt_op.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

void ResolveRuntime(CudaDrtRuntimeState& state, const nlohmann::json& drt_json) {
  ODT_Op descriptor(nlohmann::json{{"odt", drt_json}});
  descriptor.SetGlobalParams(state.cpu_params);
  state.gpu_params = GPUParamsConverter::ConvertFromCPU(state.cpu_params, state.gpu_params);
}

__global__ void DrtKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                          const GPU_TO_OUTPUT_Params* params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  auto         runtime = *params;
  const float4 source  = input[index];
  const float3 scene   = make_float3(cuda_acescc::Decode(source.x), cuda_acescc::Decode(source.y),
                                     cuda_acescc::Decode(source.z));
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

struct CudaDrtOps {
  using Device            = CudaRenderDevice;
  using Texture           = CudaBackend::Texture2D;
  using HorizontalScratch = ResourceLease<CudaBackend>;
  using LutBinding        = CudaLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteCudaDrt";

  static auto RefreshNeighborhoodAdjustment(CudaRenderDevice&, IOperatorModel& model,
                                            const ParameterSlotKey&, AdjustmentBehavior)
      -> std::optional<PendingParameterPatch> {
    return TakePendingDirtyFields(model);
  }

  static void PrepareNeighborCommands(CudaRenderDevice&, const NodeId&,
                                      std::span<const std::uint32_t>) {}

  static auto NeighborLut(CudaRenderDevice& device) -> LutBinding {
    return device.Workspace().Device().DummyLut();
  }

  static auto AcquireHorizontalScratch(CudaRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch {
    return device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
  }

  static auto HorizontalScratchTexture(HorizontalScratch& scratch) -> Texture& {
    return scratch.Texture();
  }

  static void DispatchHorizontal(CudaRenderDevice& device, const Texture& src, Texture& blur,
                                 const NeighborWork& work, std::uint32_t width,
                                 std::uint32_t height) {
    cuda_neighbor_grade::LaunchBlurHorizontal(
        device.CommandContext().Stream(), static_cast<const float4*>(src.DevicePointer()),
        static_cast<float4*>(blur.DevicePointer()), static_cast<int>(width),
        static_cast<int>(height), work.params);
  }

  static void DispatchVerticalApply(CudaRenderDevice& device, const Texture& src, const Texture& blur,
                                    Texture& dst, const LutBinding&, const NeighborWork& work,
                                    std::uint32_t width, std::uint32_t height) {
    cuda_neighbor_grade::LaunchApplyVertical(
        device.CommandContext().Stream(), static_cast<const float4*>(src.DevicePointer()),
        static_cast<const float4*>(blur.DevicePointer()), static_cast<float4*>(dst.DevicePointer()),
        static_cast<int>(width), static_cast<int>(height), work.params);
  }

  static auto AcquireOutput(CudaRenderDevice& device, const GraphValueId& id, std::uint32_t width,
                            std::uint32_t height) -> Texture& {
    return device.Workspace()
        .AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f})
        .Texture();
  }

  static auto SceneTexture(CudaRenderDevice& device, const GraphValueId& id) -> Texture& {
    auto* image = device.Workspace().Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteCudaDrt: scene image is missing");
    }
    return image->Texture();
  }

  static void CopyTexture(CudaRenderDevice& device, const GraphValueId& src,
                          const GraphValueId& dst) {
    device.Workspace().Device().CopyTexture2D(SceneTexture(device, src), SceneTexture(device, dst),
                                              device.CommandContext());
  }

  static void BindDisplayParams(CudaRenderDevice& device, const ExecutionPlan& plan,
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
      ResolveRuntime(device.DrtRuntime(), drt_json);
      const auto runtime = device.DrtRuntime().gpu_params.to_output_params_;
      arena.BindOrWritePackedSlot(key, DirtyFieldMask{kDrtDirtyBits}, runtime);
    }
    if (display_pending) {
      pending.push_back(std::move(*display_pending));
    }
  }

  static void DispatchDisplayTransform(CudaRenderDevice& device, const Texture& scene,
                                       Texture& display, const NodeId& drt_id, std::uint32_t width,
                                       std::uint32_t height) {
    auto&                  arena   = device.Workspace().Parameters();
    const ParameterSlotKey key{drt_id, AdjustmentInstanceId{"drt.output"}};
    const auto&            binding = arena.Binding(key);
    const auto*            params  = reinterpret_cast<const GPU_TO_OUTPUT_Params*>(
        static_cast<const std::byte*>(arena.DeviceBuffer().DevicePointer()) + binding.offset);
    const std::uint32_t     pixels = width * height;
    constexpr std::uint32_t block  = 256;
    DrtKernel<<<(pixels + block - 1) / block, block, 0, device.CommandContext().Stream()>>>(
        static_cast<const float4*>(scene.DevicePointer()),
        static_cast<float4*>(display.DevicePointer()), pixels, params);
  }

  static void CheckAfterEncode(CudaRenderDevice&) {
    cuda::CheckCuda(::cudaGetLastError(), "ExecuteCudaDrt: kernel launch");
  }
};

}  // namespace

auto ExecuteCudaDrt(CudaRenderDevice& device, const ExecutionPlan& plan, PipelineDocument& document)
    -> CudaDrtResult {
  const auto executed = DrtPostExecutor<CudaDrtOps>::Execute(device, plan, document);
  return {executed.output, executed.scene_post, executed.post_neighborhood_count};
}

}  // namespace alcedo
