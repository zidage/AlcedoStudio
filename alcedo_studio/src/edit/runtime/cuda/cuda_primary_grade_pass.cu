//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/cuda/cuda_adjustment_runtime.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kRuntimeParamDirtyBit = 1;
constexpr std::size_t   kMaxCurvePoints       = 8;

struct alignas(16) CudaAdjustmentParams {
  std::uint32_t behavior = 0;
  std::uint32_t count    = 0;
  float         values[30]{};
};

struct CudaAdjustmentCommand {
  std::uint32_t parameter_offset = 0;
};

struct CameraToAp1Params {
  float matrix[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float mix       = 1.0f;
};

auto MakeRuntimeParams(const IOperatorModel& model, CudaAdjustmentBehavior behavior)
    -> CudaAdjustmentParams {
  CudaAdjustmentParams result;
  result.behavior = static_cast<std::uint32_t>(behavior);
  const auto dto  = model.MakeFullDto();

  if (const auto* p = PayloadAs<ScalarFloatPayload>(dto.payload.get())) {
    result.values[0] = p->value;
    return result;
  }
  if (const auto* p = PayloadAs<Cat02WhiteBalancePayload>(dto.payload.get())) {
    result.values[0] = p->enabled ? 1.0f : 0.0f;
    result.values[1] = p->temperature_offset;
    result.values[2] = p->tint_offset;
    return result;
  }
  if (const auto* p = PayloadAs<CurvePayload>(dto.payload.get())) {
    result.count = static_cast<std::uint32_t>(std::min(p->points.size(), kMaxCurvePoints));
    for (std::uint32_t i = 0; i < result.count; ++i) {
      result.values[i * 2]     = p->points[i].x;
      result.values[i * 2 + 1] = p->points[i].y;
    }
    return result;
  }
  if (const auto* p = PayloadAs<HlsPayload>(dto.payload.get())) {
    for (int i = 0; i < kHlsHueBinCount; ++i) {
      result.values[i]      = p->hls_adj_table[i].h;
      result.values[8 + i]  = p->hls_adj_table[i].l;
      result.values[16 + i] = p->hls_adj_table[i].s;
    }
    return result;
  }
  if (const auto* p = PayloadAs<ColorWheelPayload>(dto.payload.get())) {
    const ColorWheelControl controls[3] = {p->lift, p->gamma, p->gain};
    for (int i = 0; i < 3; ++i) {
      result.values[i * 4]     = controls[i].color_offset.x;
      result.values[i * 4 + 1] = controls[i].color_offset.y;
      result.values[i * 4 + 2] = controls[i].color_offset.z;
      result.values[i * 4 + 3] = controls[i].luminance_offset;
    }
    return result;
  }
  if (const auto* p = PayloadAs<SharpenPayload>(dto.payload.get())) {
    result.values[0] = p->amount;
    result.values[1] = p->radius;
    result.values[2] = p->threshold;
    return result;
  }
  if (const auto* p = PayloadAs<LmtPayload>(dto.payload.get())) {
    result.values[0] = p->cube_path.empty() ? 0.0f : 1.0f;
    return result;
  }
  throw std::runtime_error("CUDA primary grade: Model DTO is not supported");
}

auto EnsureImage(CudaRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<CudaBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto EnsureBuffer(CudaRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> CudaBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) return *existing;
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  return *workspace.Values().Find(id);
}

__device__ auto Luma(const float3& c) -> float {
  return 0.272229f * c.x + 0.674082f * c.y + 0.053689f * c.z;
}

__device__ auto ExtrapolateCurve(float value, const CudaAdjustmentParams& p, std::uint32_t a,
                                 std::uint32_t b) -> float {
  const float x0 = p.values[a * 2];
  const float y0 = p.values[a * 2 + 1];
  const float x1 = p.values[b * 2];
  const float y1 = p.values[b * 2 + 1];
  return y0 + (value - x0) * (y1 - y0) / fmaxf(x1 - x0, 1.0e-6f);
}

__device__ auto ApplyCurve(float value, const CudaAdjustmentParams& p) -> float {
  if (p.count < 2) return value;
  if (value <= p.values[0]) return ExtrapolateCurve(value, p, 0, 1);
  for (std::uint32_t i = 1; i < p.count; ++i) {
    const float x1 = p.values[i * 2];
    if (value <= x1) {
      const float x0 = p.values[(i - 1) * 2];
      const float y0 = p.values[(i - 1) * 2 + 1];
      const float y1 = p.values[i * 2 + 1];
      const float t  = (value - x0) / fmaxf(x1 - x0, 1.0e-6f);
      return y0 + t * (y1 - y0);
    }
  }
  return ExtrapolateCurve(value, p, p.count - 2, p.count - 1);
}

__device__ auto ApplyHls(float3 c, const CudaAdjustmentParams& p) -> float3 {
  const float maximum = fmaxf(c.x, fmaxf(c.y, c.z));
  const float minimum = fminf(c.x, fminf(c.y, c.z));
  const float chroma  = maximum - minimum;
  float       hue     = 0.0f;
  if (chroma > 1.0e-6f) {
    if (maximum == c.x) {
      hue = 60.0f * fmodf((c.y - c.z) / chroma, 6.0f);
    } else if (maximum == c.y) {
      hue = 60.0f * ((c.z - c.x) / chroma + 2.0f);
    } else {
      hue = 60.0f * ((c.x - c.y) / chroma + 4.0f);
    }
  }
  if (hue < 0.0f) hue += 360.0f;
  const int   bin        = static_cast<int>((hue + 22.5f) / 45.0f) & 7;
  const float luma       = Luma(c);
  const float saturation = 1.0f + p.values[16 + bin];
  c.x                    = luma + (c.x - luma) * saturation;
  c.y                    = luma + (c.y - luma) * saturation;
  c.z                    = luma + (c.z - luma) * saturation;
  const float lightness  = p.values[8 + bin];
  c.x += lightness;
  c.y += lightness;
  c.z += lightness;
  return c;
}

__device__ auto ApplyAdjustment(float3 c, const CudaAdjustmentParams& p, std::uint32_t pixel_index,
                                float local_reference) -> float3 {
  const auto  behavior = static_cast<CudaAdjustmentBehavior>(p.behavior);
  const float value    = p.values[0];
  if (behavior == CudaAdjustmentBehavior::Cat02WhiteBalance && value != 0.0f) {
    const float temperature = p.values[1] * 0.001f;
    const float tint        = p.values[2] * 0.001f;
    c.x *= exp2f(temperature - tint * 0.5f);
    c.y *= exp2f(tint);
    c.z *= exp2f(-temperature - tint * 0.5f);
  } else if (behavior == CudaAdjustmentBehavior::Exposure) {
    const float gain = exp2f(value);
    c.x *= gain;
    c.y *= gain;
    c.z *= gain;
  } else if (behavior == CudaAdjustmentBehavior::Contrast) {
    const float scale = 1.0f + value * 0.01f;
    c.x               = (c.x - 0.18f) * scale + 0.18f;
    c.y               = (c.y - 0.18f) * scale + 0.18f;
    c.z               = (c.z - 0.18f) * scale + 0.18f;
  } else if (behavior == CudaAdjustmentBehavior::White) {
    const float gain = 1.0f + fmaxf(value, 0.0f) * 0.005f;
    c.x *= gain;
    c.y *= gain;
    c.z *= gain;
  } else if (behavior == CudaAdjustmentBehavior::Black) {
    const float offset = value * 0.001f;
    c.x += offset;
    c.y += offset;
    c.z += offset;
  } else if (behavior == CudaAdjustmentBehavior::Shadows ||
             behavior == CudaAdjustmentBehavior::Highlights ||
             behavior == CudaAdjustmentBehavior::Clarity) {
    const float l      = Luma(c);
    float       weight = behavior == CudaAdjustmentBehavior::Highlights
                             ? fminf(l / fmaxf(local_reference, 1.0e-4f), 1.0f)
                             : 1.0f - fminf(l / fmaxf(local_reference, 1.0e-4f), 1.0f);
    if (behavior == CudaAdjustmentBehavior::Clarity) weight = 0.5f - fabsf(weight - 0.5f);
    const float gain = 1.0f + value * 0.01f * weight;
    c.x *= gain;
    c.y *= gain;
    c.z *= gain;
  } else if (behavior == CudaAdjustmentBehavior::Curve) {
    c.x = ApplyCurve(c.x, p);
    c.y = ApplyCurve(c.y, p);
    c.z = ApplyCurve(c.z, p);
  } else if (behavior == CudaAdjustmentBehavior::Hls) {
    c = ApplyHls(c, p);
  } else if (behavior == CudaAdjustmentBehavior::Saturation ||
             behavior == CudaAdjustmentBehavior::Vibrance) {
    const float l = Luma(c);
    float scale   = behavior == CudaAdjustmentBehavior::Saturation ? value : 1.0f + value * 0.01f;
    if (behavior == CudaAdjustmentBehavior::Vibrance) {
      const float maximum = fmaxf(c.x, fmaxf(c.y, c.z));
      const float minimum = fminf(c.x, fminf(c.y, c.z));
      scale               = 1.0f + (scale - 1.0f) * (1.0f - fminf(maximum - minimum, 1.0f));
    }
    c.x = l + (c.x - l) * scale;
    c.y = l + (c.y - l) * scale;
    c.z = l + (c.z - l) * scale;
  } else if (behavior == CudaAdjustmentBehavior::ColorWheel) {
    const float gamma_x = fmaxf(p.values[4] + p.values[7], 1.0e-4f);
    const float gamma_y = fmaxf(p.values[5] + p.values[7], 1.0e-4f);
    const float gamma_z = fmaxf(p.values[6] + p.values[7], 1.0e-4f);
    c.x =
        copysignf(powf(fabsf(c.x + p.values[0] + p.values[3]), 1.0f / gamma_x), c.x) * p.values[8];
    c.y =
        copysignf(powf(fabsf(c.y + p.values[1] + p.values[3]), 1.0f / gamma_y), c.y) * p.values[9];
    c.z =
        copysignf(powf(fabsf(c.z + p.values[2] + p.values[3]), 1.0f / gamma_z), c.z) * p.values[10];
  } else if (behavior == CudaAdjustmentBehavior::Sharpen) {
    const float l     = Luma(c);
    const float scale = 1.0f + value * 0.0025f;
    c.x               = l + (c.x - l) * scale;
    c.y               = l + (c.y - l) * scale;
    c.z               = l + (c.z - l) * scale;
  } else if (behavior == CudaAdjustmentBehavior::Halation) {
    c.x += fmaxf(Luma(c) - 0.6f, 0.0f) * value * 0.15f;
  } else if (behavior == CudaAdjustmentBehavior::FilmGrain && value != 0.0f) {
    std::uint32_t hash = pixel_index * 747796405u + 2891336453u;
    hash               = (hash >> ((hash >> 28u) + 4u)) ^ hash;
    const float noise  = (static_cast<float>(hash & 0xffffu) / 32767.5f - 1.0f) * value * 0.02f;
    c.x += noise;
    c.y += noise;
    c.z += noise;
  }
  return c;
}

__global__ void PrimaryGradeKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                                   const unsigned char*         parameter_base,
                                   const CudaAdjustmentCommand* commands,
                                   std::uint32_t command_count, CameraToAp1Params camera,
                                   float local_reference, const std::uint8_t* mask) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  const float4 source = input[index];
  float3       c;
  c.x = camera.matrix[0] * source.x + camera.matrix[1] * source.y + camera.matrix[2] * source.z;
  c.y = camera.matrix[3] * source.x + camera.matrix[4] * source.y + camera.matrix[5] * source.z;
  c.z = camera.matrix[6] * source.x + camera.matrix[7] * source.y + camera.matrix[8] * source.z;
  const float3 converted = c;
  for (std::uint32_t i = 0; i < command_count; ++i) {
    const auto* params = reinterpret_cast<const CudaAdjustmentParams*>(
        parameter_base + commands[i].parameter_offset);
    c = ApplyAdjustment(c, *params, index, local_reference);
  }
  const float mix = camera.mix * (mask == nullptr ? 1.0f : mask[index] / 255.0f);
  c.x             = converted.x + (c.x - converted.x) * mix;
  c.y             = converted.y + (c.y - converted.y) * mix;
  c.z             = converted.z + (c.z - converted.z) * mix;
  output[index]   = make_float4(c.x, c.y, c.z, source.w);
}

}  // namespace

auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                             const RawRuntimeColorContext& color_context,
                             PipelineDocument&             document) -> CudaPrimaryGradeResult {
  (void)color_context;
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: BeginRender has not been called");
  }
  auto* grade = document.PrimaryGrade();
  if (grade == nullptr) throw std::runtime_error("ExecuteCudaPrimaryGrade: missing primary grade");
  auto* input = workspace.Images().Find(plan.develop_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: missing Develop output");
  }

  auto& arena = workspace.Parameters();
  arena.Reserve(plan.primary_grade_adjustments.size() *
                (sizeof(CudaAdjustmentParams) + ParameterArena<CudaBackend>::kSlotAlignment));
  std::vector<PendingParameterPatch> pending;
  std::vector<CudaAdjustmentCommand> commands;
  commands.reserve(plan.primary_grade_adjustments.size());
  bool                        needs_local_reference = false;
  const ParameterFieldBinding field{DirtyFieldMask{kRuntimeParamDirtyBit}, 0, 0,
                                    sizeof(CudaAdjustmentParams)};

  for (const auto& compiled : plan.primary_grade_adjustments) {
    auto* model = grade->FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      throw std::runtime_error(
          "ExecuteCudaPrimaryGrade: compiled adjustment no longer matches graph");
    }
    const auto behavior = TryResolveCudaAdjustmentBehavior(compiled.type);
    if (!behavior.has_value()) {
      continue;
    }
    needs_local_reference = needs_local_reference || IsCudaLocalToneBehavior(*behavior);
    const ParameterSlotKey key{grade->Id(), compiled.instance_id};
    const auto             runtime_params = MakeRuntimeParams(*model, *behavior);
    auto payload = std::make_shared<TypedOperatorParamPayload<CudaAdjustmentParams>>(
        compiled.type, 1, runtime_params);
    if (!arena.Contains(key)) {
      arena.BindSlot(key, sizeof(CudaAdjustmentParams), std::span{&field, 1});
      arena.InitializeFromFullDto(key, OperatorParamDto{compiled.type, 1, payload});
      if (auto first = TakePendingParameterPatch(*model)) pending.push_back(std::move(*first));
    } else if (auto change = TakePendingParameterPatch(*model)) {
      OperatorParamPatchDto patch{grade->Id(), compiled.instance_id, compiled.type,
                                  DirtyFieldMask{kRuntimeParamDirtyBit}, payload};
      arena.ApplyPatch(key, patch);
      pending.push_back(std::move(*change));
    }
    commands.push_back({arena.Binding(key).offset});
  }

  auto& context = device.CommandContext();
  arena.UploadDirty(context);
  for (auto& patch : pending) patch.Commit();

  const GraphValueId command_id{grade->Id(), PortId{"runtime.order"}};
  auto&              command_buffer = EnsureBuffer(
      workspace, command_id, std::max<std::size_t>(commands.size() * sizeof(commands[0]), 1));
  if (!commands.empty()) {
    workspace.Device().UploadBufferRange(
        command_buffer, 0,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(commands.data()),
                                   commands.size() * sizeof(commands[0])),
        context);
  }

  const GraphValueId reference_id{grade->Id(), PortId{"local_tone.reference"}};
  auto&              reference = EnsureBuffer(workspace, reference_id, sizeof(float));
  if (needs_local_reference && reference.Bytes() == sizeof(float)) {
    const float value = 0.18f;
    workspace.Device().UploadBufferRange(
        reference, 0,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), sizeof(value)),
        context);
  }

  const GraphValueId output_id{grade->Id(), PortId{"image"}};
  const auto         input_width  = input->Texture().Width();
  const auto         input_height = input->Texture().Height();
  EnsureImage(workspace, output_id, input_width, input_height);
  input        = workspace.Images().Find(plan.develop_output);
  auto* output = workspace.Images().Find(output_id);
  if (input == nullptr || output == nullptr) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: image cache changed during allocation");
  }
  const void*           input_pointer  = input->Texture().DevicePointer();
  void*                 output_pointer = output->Texture().DevicePointer();
  cudaPointerAttributes input_attributes{};
  cudaPointerAttributes output_attributes{};
  if (::cudaPointerGetAttributes(&input_attributes, input_pointer) != cudaSuccess ||
      ::cudaPointerGetAttributes(&output_attributes, output_pointer) != cudaSuccess) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: workspace image has no CUDA allocation");
  }
  CameraToAp1Params camera;
  camera.mix                       = grade->Enabled() ? grade->Mix() : 0.0f;
  const std::uint8_t* mask_pointer = nullptr;
  if (plan.primary_grade_mask) {
    auto* mask = workspace.Images().Find(plan.mask_output);
    if (mask == nullptr || mask->Empty() || mask->Texture().Format() != TextureFormat::R8 ||
        mask->Texture().Width() != input_width || mask->Texture().Height() != input_height) {
      throw std::runtime_error("ExecuteCudaPrimaryGrade: compiled mask output is missing");
    }
    mask_pointer = static_cast<const std::uint8_t*>(mask->Texture().DevicePointer());
  }
  const std::uint32_t     pixels = input_width * input_height;
  constexpr std::uint32_t block  = 256;
  PrimaryGradeKernel<<<(pixels + block - 1) / block, block, 0, context.Stream()>>>(
      static_cast<const float4*>(input_pointer), static_cast<float4*>(output_pointer), pixels,
      static_cast<const unsigned char*>(arena.DeviceBuffer().DevicePointer()),
      static_cast<const CudaAdjustmentCommand*>(command_buffer.DevicePointer()),
      static_cast<std::uint32_t>(commands.size()), camera, 0.18f, mask_pointer);
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: CUDA kernel launch failed");
  }
  return {output_id, reference.ResourceId()};
}

}  // namespace alcedo
