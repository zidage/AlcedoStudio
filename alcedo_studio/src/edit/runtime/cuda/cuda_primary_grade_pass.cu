//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "cuda_neighbor_grade.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/cuda/cuda_adjustment_runtime.hpp"
#include "edit/runtime/cuda/cuda_local_tone_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

using CudaAdjustmentParams  = GradeAdjustmentParams;
using CudaAdjustmentCommand = GradeAdjustmentCommand;

enum class GradeOpKind : std::uint8_t { Fused, Detail, LlfBarrier };

struct GradeOp {
  GradeOpKind                kind = GradeOpKind::Fused;
  std::vector<std::uint32_t> offsets;
  GradeNeighborParams        neighbor{};
};

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

auto AcquireScratch(CudaRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height)
    -> ResourceLease<CudaBackend> {
  return workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
}

auto CompactOps(std::vector<GradeOp> ops, bool local_tone_active) -> std::vector<GradeOp> {
  std::vector<GradeOp> compacted;
  compacted.reserve(ops.size());
  for (auto& op : ops) {
    if (op.kind == GradeOpKind::LlfBarrier && !local_tone_active) continue;
    if (op.kind == GradeOpKind::Fused && op.offsets.empty()) continue;
    if (op.kind == GradeOpKind::Fused && !compacted.empty() &&
        compacted.back().kind == GradeOpKind::Fused) {
      compacted.back().offsets.insert(compacted.back().offsets.end(), op.offsets.begin(),
                                      op.offsets.end());
      continue;
    }
    compacted.push_back(std::move(op));
  }
  return compacted;
}

auto LoadLut(CudaRenderDevice& device, ColorGradeNodeModel& grade) -> CudaLutBinding {
  const auto packed = TryPackGradeLut(grade);
  if (!packed.has_value()) {
    return device.Workspace().Device().DummyLut();
  }
  ContentHash hash;
  hash.MixBytes(packed->rgba);
  hash.MixU32(packed->edge);
  return device.Workspace().Device().AcquireLut(hash.Key(), packed->rgba, packed->edge,
                                                device.CommandContext());
}

auto NeighborVerticalRadius(const GradeNeighborParams& params) -> std::uint32_t {
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  if (behavior == AdjustmentBehavior::Halation) {
    return std::clamp(static_cast<std::uint32_t>(std::ceil(params.sigma_y * 3.0f)), 1U,
                      kGradeNeighborMaxTapCount - 1U);
  }
  return params.radius;
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

__device__ auto LutIndex(std::uint32_t edge, std::uint32_t x, std::uint32_t y, std::uint32_t z)
    -> std::uint32_t {
  return (z * edge + y) * edge + x;
}

__device__ auto SampleLut3d(const float4* lut, std::uint32_t edge, float u, float v, float w)
    -> float3 {
  u                   = fminf(fmaxf(u, 0.0f), 1.0f);
  v                   = fminf(fmaxf(v, 0.0f), 1.0f);
  w                   = fminf(fmaxf(w, 0.0f), 1.0f);
  const float  tex_x  = u * static_cast<float>(edge) - 0.5f;
  const float  tex_y  = v * static_cast<float>(edge) - 0.5f;
  const float  tex_z  = w * static_cast<float>(edge) - 0.5f;
  const float  max_i  = static_cast<float>(edge - 1U);
  const float  pos_x  = fminf(fmaxf(tex_x, 0.0f), max_i);
  const float  pos_y  = fminf(fmaxf(tex_y, 0.0f), max_i);
  const float  pos_z  = fminf(fmaxf(tex_z, 0.0f), max_i);
  const auto   lo_x   = static_cast<std::uint32_t>(pos_x);
  const auto   lo_y   = static_cast<std::uint32_t>(pos_y);
  const auto   lo_z   = static_cast<std::uint32_t>(pos_z);
  const auto   hi_x   = lo_x + 1U < edge ? lo_x + 1U : edge - 1U;
  const auto   hi_y   = lo_y + 1U < edge ? lo_y + 1U : edge - 1U;
  const auto   hi_z   = lo_z + 1U < edge ? lo_z + 1U : edge - 1U;
  const float  tx     = pos_x - static_cast<float>(lo_x);
  const float  ty     = pos_y - static_cast<float>(lo_y);
  const float  tz     = pos_z - static_cast<float>(lo_z);
  const float4 c000   = lut[LutIndex(edge, lo_x, lo_y, lo_z)];
  const float4 c100   = lut[LutIndex(edge, hi_x, lo_y, lo_z)];
  const float4 c010   = lut[LutIndex(edge, lo_x, hi_y, lo_z)];
  const float4 c110   = lut[LutIndex(edge, hi_x, hi_y, lo_z)];
  const float4 c001   = lut[LutIndex(edge, lo_x, lo_y, hi_z)];
  const float4 c101   = lut[LutIndex(edge, hi_x, lo_y, hi_z)];
  const float4 c011   = lut[LutIndex(edge, lo_x, hi_y, hi_z)];
  const float4 c111   = lut[LutIndex(edge, hi_x, hi_y, hi_z)];
  const float4 c00    = make_float4(c000.x + (c100.x - c000.x) * tx, c000.y + (c100.y - c000.y) * tx,
                                    c000.z + (c100.z - c000.z) * tx, c000.w + (c100.w - c000.w) * tx);
  const float4 c10    = make_float4(c010.x + (c110.x - c010.x) * tx, c010.y + (c110.y - c010.y) * tx,
                                    c010.z + (c110.z - c010.z) * tx, c010.w + (c110.w - c010.w) * tx);
  const float4 c01    = make_float4(c001.x + (c101.x - c001.x) * tx, c001.y + (c101.y - c001.y) * tx,
                                    c001.z + (c101.z - c001.z) * tx, c001.w + (c101.w - c001.w) * tx);
  const float4 c11    = make_float4(c011.x + (c111.x - c011.x) * tx, c011.y + (c111.y - c011.y) * tx,
                                    c011.z + (c111.z - c011.z) * tx, c011.w + (c111.w - c011.w) * tx);
  const float4 c0     = make_float4(c00.x + (c10.x - c00.x) * ty, c00.y + (c10.y - c00.y) * ty,
                                    c00.z + (c10.z - c00.z) * ty, c00.w + (c10.w - c00.w) * ty);
  const float4 c1     = make_float4(c01.x + (c11.x - c01.x) * ty, c01.y + (c11.y - c01.y) * ty,
                                    c01.z + (c11.z - c01.z) * ty, c01.w + (c11.w - c01.w) * ty);
  const float4 sampled = make_float4(c0.x + (c1.x - c0.x) * tz, c0.y + (c1.y - c0.y) * tz,
                                     c0.z + (c1.z - c0.z) * tz, c0.w + (c1.w - c0.w) * tz);
  return make_float3(sampled.x, sampled.y, sampled.z);
}

__device__ auto ApplyAdjustment(float3 c, const CudaAdjustmentParams& p, const float4* lut,
                                std::uint32_t lut_edge) -> float3 {
  const auto  behavior = static_cast<CudaAdjustmentBehavior>(p.behavior);
  const float value    = p.values[0];
  if (behavior == CudaAdjustmentBehavior::Cat02WhiteBalance && value != 0.0f) {
    const float temperature = p.values[1] * 0.001f;
    const float tint        = p.values[2] * 0.001f;
    c.x *= exp2f(temperature - tint * 0.5f);
    c.y *= exp2f(tint);
    c.z *= exp2f(-temperature - tint * 0.5f);
  } else if (behavior == CudaAdjustmentBehavior::Exposure) {
    const float offset = value / 17.52f;
    c.x += offset;
    c.y += offset;
    c.z += offset;
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
  } else if (behavior == CudaAdjustmentBehavior::Lmt && value != 0.0f && lut_edge > 1U &&
             lut != nullptr) {
    const float scale  = static_cast<float>(lut_edge - 1U) / static_cast<float>(lut_edge);
    const float offset = 1.0f / (2.0f * static_cast<float>(lut_edge));
    c = SampleLut3d(lut, lut_edge, c.x * scale + offset, c.y * scale + offset, c.z * scale + offset);
  }
  return c;
}

__global__ void PrimaryGradeKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                                   const unsigned char*         parameter_base,
                                   const CudaAdjustmentCommand* commands,
                                   std::uint32_t command_count, const float4* lut,
                                   std::uint32_t lut_edge) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  const float4 source = input[index];
  float3       c      = make_float3(source.x, source.y, source.z);
  for (std::uint32_t i = 0; i < command_count; ++i) {
    const auto* params = reinterpret_cast<const CudaAdjustmentParams*>(
        parameter_base + commands[i].parameter_offset);
    c = ApplyAdjustment(c, *params, lut, lut_edge);
  }
  output[index] = make_float4(c.x, c.y, c.z, source.w);
}

__global__ void FinalMixKernel(const float4* source, const float4* adjusted, float4* destination,
                               std::uint32_t pixel_count, float grade_mix,
                               const std::uint8_t* mask) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  const float  mix = grade_mix * (mask == nullptr ? 1.0f : mask[index] / 255.0f);
  const float4 a   = adjusted[index];
  const float4 s   = source[index];
  destination[index] =
      make_float4(s.x + (a.x - s.x) * mix, s.y + (a.y - s.y) * mix, s.z + (a.z - s.z) * mix, s.w);
}

}  // namespace

auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                             const PreparedRawInput& prepared, PipelineDocument& document)
    -> CudaPrimaryGradeResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: BeginRender has not been called");
  }
  const auto* compiled_grade = plan.FirstGrade();
  if (compiled_grade == nullptr) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: plan has no Color Grade");
  }
  auto* grade =
      dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(compiled_grade->node_id));
  if (grade == nullptr) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: compiled Color Grade is missing");
  }
  auto* input = workspace.Images().Find(compiled_grade->scene_input);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: missing Color Grade scene input");
  }

  auto& arena = workspace.Parameters();
  arena.Reserve(compiled_grade->adjustments.size() *
                (sizeof(CudaAdjustmentParams) + ParameterArena<CudaBackend>::kSlotAlignment));
  std::vector<PendingParameterPatch> pending;
  std::vector<GradeOp>               ops;
  ops.reserve(compiled_grade->adjustments.size());
  float                       shadows_slider    = 0.0f;
  float                       highlights_slider = 0.0f;
  const ParameterFieldBinding field{DirtyFieldMask{kGradeRuntimeParamDirtyBit}, 0, 0,
                                    sizeof(CudaAdjustmentParams)};

  auto                        FlushFused = [&]() -> GradeOp* {
    if (ops.empty() || ops.back().kind != GradeOpKind::Fused) {
      ops.push_back(GradeOp{GradeOpKind::Fused, {}, {}});
    }
    return &ops.back();
  };

  for (const auto& compiled : compiled_grade->adjustments) {
    auto* model = grade->FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      throw std::runtime_error(
          "ExecuteCudaPrimaryGrade: compiled adjustment no longer matches graph");
    }
    const auto behavior = TryResolveCudaAdjustmentBehavior(compiled.type);
    if (!behavior.has_value()) {
      throw std::runtime_error("ExecuteCudaPrimaryGrade: unregistered adjustment type '" +
                               std::string{compiled.type.Text()} + "'");
    }
    if (IsCudaLocalToneBehavior(*behavior) &&
        compiled.algorithm != CompiledAdjustmentAlgorithm::LocalLaplacian) {
      throw std::runtime_error(
          "ExecuteCudaPrimaryGrade: Shadows/Highlights were not compiled for LLF");
    }
    if (compiled.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian &&
        !IsCudaLocalToneBehavior(*behavior)) {
      throw std::runtime_error(
          "ExecuteCudaPrimaryGrade: non-local adjustment was compiled for LLF");
    }
    const ParameterSlotKey key{grade->Id(), compiled.instance_id};
    const auto             runtime_params = MakeGradeRuntimeParams(*model, *behavior);
    auto payload = std::make_shared<TypedOperatorParamPayload<CudaAdjustmentParams>>(
        compiled.type, 1, runtime_params);
    if (!arena.Contains(key)) {
      arena.BindSlot(key, sizeof(CudaAdjustmentParams), std::span{&field, 1});
      arena.InitializeFromFullDto(key, OperatorParamDto{compiled.type, 1, payload});
      if (auto first = TakePendingParameterPatch(*model)) pending.push_back(std::move(*first));
    } else if (auto change = TakePendingParameterPatch(*model)) {
      OperatorParamPatchDto patch{grade->Id(), compiled.instance_id, compiled.type,
                                  DirtyFieldMask{kGradeRuntimeParamDirtyBit}, payload};
      arena.ApplyPatch(key, patch);
      pending.push_back(std::move(*change));
    }
    if (compiled.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian) {
      if (*behavior == CudaAdjustmentBehavior::Shadows) {
        shadows_slider = runtime_params.values[0];
      } else {
        highlights_slider = runtime_params.values[0];
      }
      if (ops.empty() || ops.back().kind != GradeOpKind::LlfBarrier) {
        ops.push_back(GradeOp{GradeOpKind::LlfBarrier, {}, {}});
      }
      continue;
    }

    if (compiled.algorithm == CompiledAdjustmentAlgorithm::Neighborhood ||
        IsNeighborhoodBehavior(*behavior)) {
      auto neighbor = MakeGradeNeighborParams(*model, *behavior, plan.geometry);
      if (neighbor.enabled != 0U) {
        ops.push_back(GradeOp{GradeOpKind::Detail, {}, neighbor});
      }
      continue;
    }
    if (compiled.algorithm != CompiledAdjustmentAlgorithm::Pointwise) {
      throw std::runtime_error("ExecuteCudaPrimaryGrade: unsupported grade algorithm");
    }
    FlushFused()->offsets.push_back(arena.Binding(key).offset);
  }

  const bool local_tone_active = local_tone_mapping::ShouldRun(
      shadows_slider * local_tone_mapping::kHighlightStrengthScale / 80.0f,
      -highlights_slider * local_tone_mapping::kHighlightStrengthScale / 100.0f);
  ops           = CompactOps(std::move(ops), local_tone_active);

  auto& context = device.CommandContext();
  arena.UploadDirty(context);
  for (auto& patch : pending) patch.Commit();

  std::vector<CudaAdjustmentCommand> commands;
  commands.reserve(compiled_grade->adjustments.size());
  std::vector<std::uint32_t> command_starts;
  command_starts.reserve(ops.size());
  for (const auto& op : ops) {
    command_starts.push_back(static_cast<std::uint32_t>(commands.size()));
    for (const auto offset : op.offsets) commands.push_back({offset});
  }
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

  const GraphValueId  output_id    = compiled_grade->scene_output;
  const auto          input_width  = input->Texture().Width();
  const auto          input_height = input->Texture().Height();
  const std::uint8_t* mask_pointer = nullptr;
  if (compiled_grade->mask) {
    auto* mask = workspace.Images().Find(compiled_grade->mask_output);
    if (mask == nullptr || mask->Empty() || mask->Texture().Format() != TextureFormat::R8 ||
        mask->Texture().Width() != input_width || mask->Texture().Height() != input_height) {
      throw std::runtime_error("ExecuteCudaPrimaryGrade: compiled mask output is missing");
    }
    mask_pointer = static_cast<const std::uint8_t*>(mask->Texture().DevicePointer());
  }
  const std::uint32_t     pixels = input_width * input_height;
  constexpr std::uint32_t block  = 256;
  const auto*             device_commands =
      static_cast<const CudaAdjustmentCommand*>(command_buffer.DevicePointer());
  const auto* parameter_base =
      static_cast<const unsigned char*>(arena.DeviceBuffer().DevicePointer());
  const auto lut = LoadLut(device, *grade);

  CudaLocalToneResult local_tone;

  if (ops.empty()) {
    auto& output = EnsureImage(workspace, output_id, input_width, input_height);
    input        = workspace.Images().Find(compiled_grade->scene_input);
    workspace.Device().CopyTexture2D(input->Texture(), output.Texture(), context);
    return {output_id, lut.resource_id, 0, false, false};
  }

  const float        grade_mix        = grade->Enabled() ? grade->Mix() : 0.0f;
  const bool         skip_mix         = grade_mix == 1.0f && !compiled_grade->mask.has_value();
  std::size_t        remaining_writes = ops.size() + (skip_mix ? 0U : 1U);
  const GraphValueId ping_id{grade->Id(), PortId{"runtime.ping"}};
  const GraphValueId pong_id{grade->Id(), PortId{"runtime.pong"}};
  GraphValueId       current_id = compiled_grade->scene_input;

  auto               Resolve    = [&](const GraphValueId& id) -> CudaBackend::Texture2D& {
    auto* image = workspace.Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteCudaPrimaryGrade: stage image is missing");
    }
    return image->Texture();
  };
  auto AllocateDest = [&]() -> GraphValueId {
    if (remaining_writes == 0) {
      throw std::runtime_error("ExecuteCudaPrimaryGrade: destination count underflow");
    }
    --remaining_writes;
    GraphValueId destination = output_id;
    if (remaining_writes != 0) {
      destination = current_id == ping_id ? pong_id : ping_id;
    }
    (void)EnsureImage(workspace, destination, input_width, input_height);
    return destination;
  };

  const dim3 neighbor_block{16, 16};
  const dim3 neighbor_grid{(input_width + neighbor_block.x - 1) / neighbor_block.x,
                           (input_height + neighbor_block.y - 1) / neighbor_block.y};
  for (std::size_t index = 0; index < ops.size(); ++index) {
    const auto& op      = ops[index];
    const auto  dest_id = AllocateDest();
    if (op.kind == GradeOpKind::Detail) {
      auto  blur_horizontal = AcquireScratch(workspace, input_width, input_height);
      auto& src             = Resolve(current_id);
      auto& dest            = Resolve(dest_id);
      cuda_neighbor_grade::BlurHorizontal<<<neighbor_grid, neighbor_block, 0, context.Stream()>>>(
          static_cast<const float4*>(src.DevicePointer()),
          static_cast<float4*>(blur_horizontal.Texture().DevicePointer()),
          static_cast<int>(input_width), static_cast<int>(input_height), op.neighbor);
      const auto vertical_radius = NeighborVerticalRadius(op.neighbor);
      const auto shared_bytes    = static_cast<std::size_t>(neighbor_block.x) *
                                (neighbor_block.y + 2U * vertical_radius) * sizeof(float4);
      cuda_neighbor_grade::
          ApplyVertical<<<neighbor_grid, neighbor_block, shared_bytes, context.Stream()>>>(
              static_cast<const float4*>(src.DevicePointer()),
              static_cast<const float4*>(blur_horizontal.Texture().DevicePointer()),
              static_cast<float4*>(dest.DevicePointer()), static_cast<int>(input_width),
              static_cast<int>(input_height), op.neighbor);
    } else if (op.kind == GradeOpKind::LlfBarrier) {
      local_tone = ExecuteCudaLocalTone(
          device, current_id, dest_id, grade->Id(), input_width, input_height, shadows_slider,
          highlights_slider, plan.geometry, HashLlfReferenceKey(plan, prepared, document));
    } else {
      auto& src  = Resolve(current_id);
      auto& dest = Resolve(dest_id);
      PrimaryGradeKernel<<<(pixels + block - 1) / block, block, 0, context.Stream()>>>(
          static_cast<const float4*>(src.DevicePointer()),
          static_cast<float4*>(dest.DevicePointer()), pixels, parameter_base,
          device_commands + command_starts[index], static_cast<std::uint32_t>(op.offsets.size()),
          static_cast<const float4*>(lut.device_pointer), lut.edge_size);
    }
    current_id = dest_id;
  }

  if (!skip_mix) {
    const auto dest_id     = AllocateDest();
    auto&      source      = Resolve(plan.develop_output);
    auto&      adjusted    = Resolve(current_id);
    auto&      destination = Resolve(dest_id);
    FinalMixKernel<<<(pixels + block - 1) / block, block, 0, context.Stream()>>>(
        static_cast<const float4*>(source.DevicePointer()),
        static_cast<const float4*>(adjusted.DevicePointer()),
        static_cast<float4*>(destination.DevicePointer()), pixels, grade_mix, mask_pointer);
    current_id = dest_id;
  }

  if (current_id != output_id || remaining_writes != 0) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: grade output scheduling failed");
  }
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: CUDA kernel launch failed");
  }
  return {output_id, lut.resource_id, local_tone.reference_resource_id,
          local_tone.rebuilt_reference, local_tone.sampled_canonical_reference};
}

}  // namespace alcedo
