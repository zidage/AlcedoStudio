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
#include <string>
#include <vector>

#include "cuda_acescc.cuh"
#include "cuda_neighbor_grade.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/cuda/cuda_adjustment_runtime.hpp"
#include "edit/runtime/cuda/cuda_local_tone_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/grade_executor.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/local_tone_executor.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

using CudaAdjustmentParams  = GradeAdjustmentParams;
using CudaAdjustmentCommand = GradeAdjustmentCommand;

auto EnsureBuffer(CudaRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> CudaBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) return *existing;
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  return *workspace.Values().Find(id);
}

auto AcquireCudaScratch(CudaRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height)
    -> ResourceLease<CudaBackend> {
  return workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
}

auto LoadCudaGradeLut(CudaRenderDevice& device, ColorGradeNodeModel& grade) -> CudaLutBinding {
  const auto packed = TryPackGradeLut(grade);
  if (packed == nullptr) {
    return device.Workspace().Device().DummyLut();
  }
  return device.Workspace().Device().AcquireLut(packed->key, packed->rgba, packed->edge,
                                                device.CommandContext());
}

__device__ auto Luma(const float3& c) -> float {
  return 0.272229f * c.x + 0.674082f * c.y + 0.053689f * c.z;
}

// Contrast is an S curve on OkLab lightness of scene-linear AP1, so it changes tone without
// shifting hue or overall brightness:
//   - AP1 -> LMS uses a white-normalized matrix (each row sums to 1, a von Kries adaptation in
//     OkLab LMS), so AP1 neutrals have a = b = 0 and L = cbrt(Y).
//   - x = log2(L^3 / 0.18) is stops from 18% grey. f(x) = x + (k - 1) * w * tanh(x / w) with
//     k = 2^(contrast / 100) and w = 2.5 stops: slope k at mid grey, slope 1 far from it, and a
//     shift bounded by +/-(k - 1) * w stops. As a lightness gain this is
//     L' = L * 2^((k - 1) * w * tanh(x / w) / 3); L <= 0 uses the tanh = -1 limit.
//   - Chroma follows contrast gently: a, b scale by sqrt(k) * min(gain, 1). Where the curve
//     darkens, chroma shrinks with lightness so saturation (C / L) rises by only sqrt(k); keeping
//     chroma fixed there would push noisy near-black colors out of gamut. Hue is unchanged.
// The Metal and OpenCL grade shaders implement the same math, and GPU tests compare all three
// against tests/edit/runtime/oklab_contrast_reference.hpp.
__device__ auto LinearAp1ToOkLab(const float3& c) -> float3 {
  const float l  = 0.6341104672f * c.x + 0.3489495087f * c.y + 0.0169400240f * c.z;
  const float m  = 0.2754060131f * c.x + 0.6327713632f * c.y + 0.0918226237f * c.z;
  const float s  = 0.1056775254f * c.x + 0.1971481306f * c.y + 0.6971743440f * c.z;
  const float l_ = cbrtf(l);
  const float m_ = cbrtf(m);
  const float s_ = cbrtf(s);
  return make_float3(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                     1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                     0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

__device__ auto OkLabToLinearAp1(const float3& lab) -> float3 {
  const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
  const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
  const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
  const float l  = l_ * l_ * l_;
  const float m  = m_ * m_ * m_;
  const float s  = s_ * s_ * s_;
  return make_float3(2.0693822907f * l - 1.1736821545f * m + 0.1042998638f * s,
                     -0.8917480677f * l + 2.1537429245f * m - 0.2619948569f * s,
                     -0.0615064732f * l - 0.4311325686f * m + 1.4926390418f * s);
}

__device__ auto ApplyOkLabContrast(const float3& acescc, float contrast) -> float3 {
  constexpr float kPivotLightness  = 0.5646216f;  // cbrt(0.18)
  constexpr float kCurveWidthStops = 2.5f;
  const float     slope            = exp2f(contrast * 0.01f);
  float3          lab              = LinearAp1ToOkLab(make_float3(
      cuda_acescc::Decode(acescc.x), cuda_acescc::Decode(acescc.y), cuda_acescc::Decode(acescc.z)));
  const float     shape =
      lab.x > 0.0f ? tanhf(3.0f * log2f(lab.x / kPivotLightness) / kCurveWidthStops) : -1.0f;
  const float gain         = exp2f((slope - 1.0f) * kCurveWidthStops * shape / 3.0f);
  const float chroma_scale = sqrtf(slope) * fminf(gain, 1.0f);
  lab                      = make_float3(lab.x * gain, lab.y * chroma_scale, lab.z * chroma_scale);
  const float3 linear_ap1  = OkLabToLinearAp1(lab);
  return make_float3(cuda_acescc::Encode(linear_ap1.x), cuda_acescc::Encode(linear_ap1.y),
                     cuda_acescc::Encode(linear_ap1.z));
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
  if (chroma <= 1.0e-6f) return c;

  float sum_h = 0.0f, sum_l = 0.0f, sum_s = 0.0f, sum_weight = 0.0f;
  for (int i = 0; i < 8; ++i) {
    const float difference = fabsf(hue - p.values[i]);
    const float distance   = fminf(difference, 360.0f - difference);
    const float width      = fmaxf(p.values[32 + i], 1.0f);
    const float weight     = exp2f(-distance * distance / (width * width));
    sum_h += p.values[8 + i * 3] * weight;
    sum_l += p.values[8 + i * 3 + 1] * weight;
    sum_s += p.values[8 + i * 3 + 2] * weight;
    sum_weight += weight;
  }
  if (sum_weight <= 1.0e-6f) return c;
  const float inv_weight = 1.0f / sum_weight;
  const float adj_h      = sum_h * inv_weight;
  const float adj_l      = sum_l * inv_weight;
  const float chroma_adj = sum_s * inv_weight;
  if (fabsf(adj_h) <= 1.0e-6f && fabsf(adj_l) <= 1.0e-6f && fabsf(chroma_adj) <= 1.0e-6f) {
    return c;
  }
  const float  hue_shift    = adj_h * 2.25f * 0.017453292519943295f;
  const float  lightness    = adj_l * 1.125f;
  const float  chroma_scale = exp2f(chroma_adj * 2.25f * (chroma_adj >= 0.0f ? 4.5f : 3.25f));
  const float  luma         = Luma(c) + lightness;
  const float  i            = 0.596f * c.x - 0.274f * c.y - 0.322f * c.z;
  const float  q            = 0.211f * c.x - 0.523f * c.y + 0.312f * c.z;
  const float  rotated_i    = (i * cosf(hue_shift) - q * sinf(hue_shift)) * chroma_scale;
  const float  rotated_q    = (i * sinf(hue_shift) + q * cosf(hue_shift)) * chroma_scale;
  return make_float3(luma + 0.956f * rotated_i + 0.621f * rotated_q,
                     luma - 0.272f * rotated_i - 0.647f * rotated_q,
                     luma - 1.106f * rotated_i + 1.703f * rotated_q);
}

__device__ auto LutIndex(std::uint32_t edge, std::uint32_t x, std::uint32_t y, std::uint32_t z)
    -> std::uint32_t {
  return (z * edge + y) * edge + x;
}

__device__ auto SampleLut3d(const float4* lut, std::uint32_t edge, float u, float v, float w)
    -> float3 {
  u                  = fminf(fmaxf(u, 0.0f), 1.0f);
  v                  = fminf(fmaxf(v, 0.0f), 1.0f);
  w                  = fminf(fmaxf(w, 0.0f), 1.0f);
  const float  tex_x = u * static_cast<float>(edge) - 0.5f;
  const float  tex_y = v * static_cast<float>(edge) - 0.5f;
  const float  tex_z = w * static_cast<float>(edge) - 0.5f;
  const float  max_i = static_cast<float>(edge - 1U);
  const float  pos_x = fminf(fmaxf(tex_x, 0.0f), max_i);
  const float  pos_y = fminf(fmaxf(tex_y, 0.0f), max_i);
  const float  pos_z = fminf(fmaxf(tex_z, 0.0f), max_i);
  const auto   lo_x  = static_cast<std::uint32_t>(pos_x);
  const auto   lo_y  = static_cast<std::uint32_t>(pos_y);
  const auto   lo_z  = static_cast<std::uint32_t>(pos_z);
  const auto   hi_x  = lo_x + 1U < edge ? lo_x + 1U : edge - 1U;
  const auto   hi_y  = lo_y + 1U < edge ? lo_y + 1U : edge - 1U;
  const auto   hi_z  = lo_z + 1U < edge ? lo_z + 1U : edge - 1U;
  const float  tx    = pos_x - static_cast<float>(lo_x);
  const float  ty    = pos_y - static_cast<float>(lo_y);
  const float  tz    = pos_z - static_cast<float>(lo_z);
  const float4 c000  = lut[LutIndex(edge, lo_x, lo_y, lo_z)];
  const float4 c100  = lut[LutIndex(edge, hi_x, lo_y, lo_z)];
  const float4 c010  = lut[LutIndex(edge, lo_x, hi_y, lo_z)];
  const float4 c110  = lut[LutIndex(edge, hi_x, hi_y, lo_z)];
  const float4 c001  = lut[LutIndex(edge, lo_x, lo_y, hi_z)];
  const float4 c101  = lut[LutIndex(edge, hi_x, lo_y, hi_z)];
  const float4 c011  = lut[LutIndex(edge, lo_x, hi_y, hi_z)];
  const float4 c111  = lut[LutIndex(edge, hi_x, hi_y, hi_z)];
  const float4 c00   = make_float4(c000.x + (c100.x - c000.x) * tx, c000.y + (c100.y - c000.y) * tx,
                                   c000.z + (c100.z - c000.z) * tx, c000.w + (c100.w - c000.w) * tx);
  const float4 c10   = make_float4(c010.x + (c110.x - c010.x) * tx, c010.y + (c110.y - c010.y) * tx,
                                   c010.z + (c110.z - c010.z) * tx, c010.w + (c110.w - c010.w) * tx);
  const float4 c01   = make_float4(c001.x + (c101.x - c001.x) * tx, c001.y + (c101.y - c001.y) * tx,
                                   c001.z + (c101.z - c001.z) * tx, c001.w + (c101.w - c001.w) * tx);
  const float4 c11   = make_float4(c011.x + (c111.x - c011.x) * tx, c011.y + (c111.y - c011.y) * tx,
                                   c011.z + (c111.z - c011.z) * tx, c011.w + (c111.w - c011.w) * tx);
  const float4 c0    = make_float4(c00.x + (c10.x - c00.x) * ty, c00.y + (c10.y - c00.y) * ty,
                                   c00.z + (c10.z - c00.z) * ty, c00.w + (c10.w - c00.w) * ty);
  const float4 c1    = make_float4(c01.x + (c11.x - c01.x) * ty, c01.y + (c11.y - c01.y) * ty,
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
    // values[1..9]: row-major CAT02 adaptation in linear AP1 (resolved on the CPU).
    const float3 linear = make_float3(cuda_acescc::Decode(c.x), cuda_acescc::Decode(c.y),
                                      cuda_acescc::Decode(c.z));
    const float* m      = p.values + 1;
    c = make_float3(cuda_acescc::Encode(m[0] * linear.x + m[1] * linear.y + m[2] * linear.z),
                    cuda_acescc::Encode(m[3] * linear.x + m[4] * linear.y + m[5] * linear.z),
                    cuda_acescc::Encode(m[6] * linear.x + m[7] * linear.y + m[8] * linear.z));
  } else if (behavior == CudaAdjustmentBehavior::Exposure) {
    const float offset = value / 17.52f;
    c.x += offset;
    c.y += offset;
    c.z += offset;
  } else if (behavior == CudaAdjustmentBehavior::Contrast && value != 0.0f) {
    c = ApplyOkLabContrast(c, value);
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
    float scale = behavior == CudaAdjustmentBehavior::Saturation ? value : 1.0f + value * 0.01f;
    if (behavior == CudaAdjustmentBehavior::Vibrance) {
      const float maximum = fmaxf(c.x, fmaxf(c.y, c.z));
      const float minimum = fminf(c.x, fminf(c.y, c.z));
      scale               = 1.0f + (scale - 1.0f) * (1.0f - fminf(maximum - minimum, 1.0f));
    }
    const float l = Luma(c);
    if (scale > 1.5f) {
      // Retain the established luma-pivot response, but limit a single saturation operation to
      // less than two stops of new log-domain peak. This only engages for pathological channel
      // separation and leaves ordinary grading, including the default OpenDRT setup, unchanged.
      const float peak       = fmaxf(c.x, fmaxf(c.y, c.z));
      const float peak_raise = (peak - l) * (scale - 1.0f);
      if (peak_raise > 0.1f) scale = 1.0f + 0.1f / fmaxf(peak - l, 1.0e-6f);
    }
    c.x           = l + (c.x - l) * scale;
    c.y           = l + (c.y - l) * scale;
    c.z           = l + (c.z - l) * scale;
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
    c                  = SampleLut3d(lut, lut_edge, c.x * scale + offset, c.y * scale + offset,
                                     c.z * scale + offset);
  }
  return c;
}

__device__ auto MixGradePixel(const float4& original, const float4& adjusted, float grade_mix,
                              const std::uint8_t* mask, std::uint32_t index) -> float4 {
  const float mix = grade_mix * (mask == nullptr ? 1.0f : mask[index] / 255.0f);
  return make_float4(original.x + (adjusted.x - original.x) * mix,
                     original.y + (adjusted.y - original.y) * mix,
                     original.z + (adjusted.z - original.z) * mix, original.w);
}

__global__ void PrimaryGradeKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                                   const unsigned char*         parameter_base,
                                   const CudaAdjustmentCommand* commands,
                                   std::uint32_t command_count, const float4* lut,
                                   std::uint32_t lut_edge, const float4* mix_source, float grade_mix,
                                   const std::uint8_t* mask) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  const float4 source = input[index];
  float3       c      = make_float3(source.x, source.y, source.z);
  for (std::uint32_t i = 0; i < command_count; ++i) {
    const auto* params = reinterpret_cast<const CudaAdjustmentParams*>(
        parameter_base + commands[i].parameter_offset);
    c = ApplyAdjustment(c, *params, lut, lut_edge);
  }
  const float4 adjusted = make_float4(c.x, c.y, c.z, source.w);
  if (mix_source == nullptr) {
    output[index] = adjusted;
    return;
  }
  output[index] = MixGradePixel(mix_source[index], adjusted, grade_mix, mask, index);
}

auto MaskPointer(CudaRenderDevice& device, const GraphValueId* mask_id, std::uint32_t width,
                 std::uint32_t height) -> const std::uint8_t* {
  if (mask_id == nullptr) {
    return nullptr;
  }
  auto* mask = device.Workspace().Images().Find(*mask_id);
  if (mask == nullptr || mask->Empty() || mask->Texture().Format() != TextureFormat::R8 ||
      mask->Texture().Width() != width || mask->Texture().Height() != height) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: compiled mask output is missing");
  }
  return static_cast<const std::uint8_t*>(mask->Texture().DevicePointer());
}

void LaunchPointwise(CudaRenderDevice& device, const FrameSceneBinding& src,
                     const FrameSceneBinding& dst, const FrameSceneBinding* mix_source,
                     const CudaLutBinding& lut, const NodeId& grade_id, std::uint32_t command_start,
                     std::uint32_t command_count, float mix, const GraphValueId* mask_id,
                     std::uint32_t width, std::uint32_t height) {
  auto& workspace = device.Workspace();
  auto* commands  = workspace.Values().Find(GraphValueId{grade_id, PortId{"runtime.order"}});
  if (commands == nullptr || command_count == 0) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: missing fused command buffer");
  }
  const auto  pixels = width * height;
  const auto* device_commands =
      static_cast<const CudaAdjustmentCommand*>(commands->DevicePointer());
  const auto* parameter_base =
      static_cast<const unsigned char*>(workspace.Parameters().DeviceBuffer().DevicePointer());
  auto&       src_tex = CudaSceneTexture(device, src);
  auto&       dst_tex = CudaSceneTexture(device, dst);
  const auto* mix_ptr =
      mix_source == nullptr ? nullptr
                            : static_cast<const float4*>(CudaSceneTexture(device, *mix_source)
                                                             .DevicePointer());
  constexpr std::uint32_t block = 256;
  PrimaryGradeKernel<<<(pixels + block - 1) / block, block, 0, device.CommandContext().Stream()>>>(
      static_cast<const float4*>(src_tex.DevicePointer()),
      static_cast<float4*>(dst_tex.DevicePointer()), pixels, parameter_base,
      device_commands + command_start, command_count, static_cast<const float4*>(lut.device_pointer),
      lut.edge_size, mix_ptr, mix, MaskPointer(device, mask_id, width, height));
}

struct CudaGradeOps {
  using Device            = CudaRenderDevice;
  using Backend           = CudaBackend;
  using Texture           = CudaBackend::Texture2D;
  using HorizontalScratch = ResourceLease<CudaBackend>;
  using LutBinding        = CudaLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteCudaPrimaryGrade";

  static auto BindingWidth(CudaRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return CudaSceneWidth(device, binding);
  }

  static auto BindingHeight(CudaRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return CudaSceneHeight(device, binding);
  }

  static auto UploadFusedCommands(CudaRenderDevice& device, const NodeId& grade_id,
                                  const std::vector<std::uint32_t>& fused_offsets)
      -> std::uint32_t {
    if (fused_offsets.empty()) {
      return 0;
    }
    auto&              workspace = device.Workspace();
    const GraphValueId command_id{grade_id, PortId{"runtime.order"}};
    const auto         bytes = fused_offsets.size() * sizeof(fused_offsets[0]);
    auto&              buffer =
        EnsureBuffer(workspace, command_id, std::max<std::size_t>(bytes, sizeof(std::uint32_t)));
    workspace.Device().UploadBufferRange(
        buffer, 0,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(fused_offsets.data()), bytes),
        device.CommandContext());
    return static_cast<std::uint32_t>(bytes);
  }

  static auto LoadLut(CudaRenderDevice& device, ColorGradeNodeModel& grade) -> CudaLutBinding {
    return LoadCudaGradeLut(device, grade);
  }

  static auto LutResourceId(const CudaLutBinding& lut) -> std::uint64_t { return lut.resource_id; }

  static void DispatchPointwise(CudaRenderDevice& device, const FrameSceneBinding& src,
                                const FrameSceneBinding& dst, const CudaLutBinding& lut,
                                const NodeId& grade_id, std::uint32_t command_start,
                                std::uint32_t command_count, std::uint32_t width,
                                std::uint32_t height) {
    LaunchPointwise(device, src, dst, nullptr, lut, grade_id, command_start, command_count, 1.0f,
                    nullptr, width, height);
  }

  static void DispatchPointwiseWithMix(CudaRenderDevice& device, const FrameSceneBinding& src,
                                       const FrameSceneBinding& dst,
                                       const FrameSceneBinding& original, const CudaLutBinding& lut,
                                       const NodeId& grade_id, std::uint32_t command_start,
                                       std::uint32_t command_count, float mix,
                                       const GraphValueId* mask_id, std::uint32_t width,
                                       std::uint32_t height) {
    LaunchPointwise(device, src, dst, &original, lut, grade_id, command_start, command_count, mix,
                    mask_id, width, height);
  }

  static auto AcquireHorizontalScratch(CudaRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch {
    return AcquireCudaScratch(device.Workspace(), width, height);
  }

  static void DispatchHorizontal(CudaRenderDevice& device, const FrameSceneBinding& src,
                                 HorizontalScratch& scratch, const NeighborWork& work,
                                 std::uint32_t width, std::uint32_t height) {
    auto& src_tex = CudaSceneTexture(device, src);
    cuda_neighbor_grade::LaunchBlurHorizontal(
        device.CommandContext().Stream(), static_cast<const float4*>(src_tex.DevicePointer()),
        static_cast<float4*>(scratch.Texture().DevicePointer()), static_cast<int>(width),
        static_cast<int>(height), work.params);
  }

  static void DispatchVerticalApply(CudaRenderDevice& device, const FrameSceneBinding& src,
                                    HorizontalScratch& scratch, const FrameSceneBinding& dst,
                                    const FrameSceneBinding& original, const LutBinding&,
                                    const NeighborWork& work, float mix, const GraphValueId* mask_id,
                                    std::uint32_t width, std::uint32_t height) {
    auto& src_tex  = CudaSceneTexture(device, src);
    auto& dst_tex  = CudaSceneTexture(device, dst);
    auto& orig_tex = CudaSceneTexture(device, original);
    cuda_neighbor_grade::LaunchApplyVertical(
        device.CommandContext().Stream(), static_cast<const float4*>(src_tex.DevicePointer()),
        static_cast<const float4*>(scratch.Texture().DevicePointer()),
        static_cast<float4*>(dst_tex.DevicePointer()), static_cast<const float4*>(orig_tex.DevicePointer()),
        mix, MaskPointer(device, mask_id, width, height), static_cast<int>(width),
        static_cast<int>(height), work.params);
  }

  static auto ExecuteLocalTone(CudaRenderDevice& device, const FrameSceneBinding& src,
                               const FrameSceneBinding& dest, const FrameSceneBinding& original,
                               const NodeId& grade_id, float shadows_slider,
                               float highlights_slider, const ResolvedRenderGeometry& geometry,
                               float mix, const GraphValueId* mask_id) -> CudaLocalToneResult {
    return ExecuteCudaLocalTone(device, src, dest, original, grade_id, shadows_slider,
                                highlights_slider, geometry, mix, mask_id);
  }

  static void CheckAfterEncode(CudaRenderDevice&) {
    if (::cudaGetLastError() != cudaSuccess) {
      throw std::runtime_error("ExecuteCudaPrimaryGrade: CUDA kernel launch failed");
    }
  }
};

}  // namespace

auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                             const PreparedRawInput& prepared, PipelineDocument& document,
                             const CompiledGradeNode& compiled_grade_node,
                             const FrameSceneBinding& scene) -> CudaPrimaryGradeResult {
  const auto executed = GradeExecutor<CudaGradeOps>::Execute(device, plan, prepared, document,
                                                             compiled_grade_node, scene);
  CudaPrimaryGradeResult result;
  result.output                                 = executed.output;
  result.output_binding                         = executed.output_binding;
  result.lut_resource_id                        = executed.lut_resource_id;
  result.local_tone_reference_resource_id       = executed.local_tone_reference_resource_id;
  result.local_tone_rebuilt_reference           = executed.local_tone_rebuilt_reference;
  result.local_tone_sampled_canonical_reference = executed.local_tone_sampled_canonical_reference;
  result.pointwise_dispatch_count               = executed.pointwise_dispatch_count;
  result.detail_pass_count                      = executed.detail_pass_count;
  result.local_tone_pass_count                  = executed.local_tone_pass_count;
  return result;
}

auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                             const PreparedRawInput& prepared, PipelineDocument& document)
    -> CudaPrimaryGradeResult {
  if (plan.grade_nodes.empty()) {
    throw std::runtime_error("ExecuteCudaPrimaryGrade: plan has no Color Grade");
  }
  device.Workspace().PrepareResultValidity(plan, document, prepared);
  const ImageExtent extent{plan.geometry.render_extent.width, plan.geometry.render_extent.height};
  device.Workspace().EnsureSceneWorkImages(extent);
  FrameSceneBinding scene = FrameSceneBinding::CachedImage(plan.develop_output);
  CudaPrimaryGradeResult last{};
  for (const auto& compiled_grade : plan.grade_nodes) {
    last  = ExecuteCudaPrimaryGrade(device, plan, prepared, document, compiled_grade, scene);
    scene = last.output_binding;
  }
  return last;
}

}  // namespace alcedo
