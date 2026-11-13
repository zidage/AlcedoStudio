//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_encode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "decoders/processor/operators/gpu/opencl_raw_programs.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo::OpenCL {
namespace {

using opencl::OpenClBufferView;
using opencl::OpenClEncodeQueue;
using RawProcessor::kCfaClamp01KernelName;
using RawProcessor::kCopyRgbaCropInverseOrientKernelName;
using RawProcessor::kCopyRgbCropInverseOrientKernelName;
using RawProcessor::kCoreProgramName;
using RawProcessor::kCvtRefSpaceProgramName;
using RawProcessor::kDebayerRcdProgramName;
using RawProcessor::kHighlightProgramName;
using RawProcessor::kHlrBuildMaskKernelName;
using RawProcessor::kHlrBuildMaskPlanarKernelName;
using RawProcessor::kHlrChrominanceContribKernelName;
using RawProcessor::kHlrChrominanceContribPlanarKernelName;
using RawProcessor::kHlrDilateMaskKernelName;
using RawProcessor::kHlrReconstructFromStatsKernelName;
using RawProcessor::kHlrReconstructFromStatsPlanarPackKernelName;
using RawProcessor::kPackPlanesCropInverseOrientKernelName;
using RawProcessor::kRcdGreenAtRbKernelName;
using RawProcessor::kRcdInitAndVhKernelName;
using RawProcessor::kRcdPqDirKernelName;
using RawProcessor::kRcdRbAtGKernelName;
using RawProcessor::kRcdRbAtRbKernelName;
using RawProcessor::kToLinearRefKernelName;
using RawProcessor::kXTransGreenKernelName;
using RawProcessor::kXTransProgramName;
using RawProcessor::kXTransRgbaKernelName;

struct WBParams {
  float black_level[4];
  float white_level[4];
  float wb_multipliers[4];
  int   apply_white_balance;
  int   black_tile_width;
  int   black_tile_height;
  float pattern_black[36];
};

struct PatternParams {
  int width;
  int height;
  int tile_width;
  int tile_height;
  int raw_fc[36];
};

struct SinglePlaneParams {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t stride;
  std::uint32_t rgb_fc[4];
};

struct XTransParams {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t tile_width;
  std::uint32_t tile_height;
  std::uint32_t passes;
  std::uint32_t green_radius;
  std::uint32_t rb_radius;
  std::uint32_t rgb_fc[36];
};

struct HighlightParams {
  float         clips[4];
  float         clipdark[4];
  float         chrominance[4];
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t stride;
};

struct PackOrientParams {
  std::uint32_t src_x;
  std::uint32_t src_y;
  std::uint32_t src_width;
  std::uint32_t src_height;
  std::uint32_t dst_width;
  std::uint32_t dst_height;
  std::uint32_t flip;
  float         scale_r;
  float         scale_g;
  float         scale_b;
  std::uint32_t plane_stride;
  std::uint32_t src_stride;
};

constexpr float kHilightMagic = 0.987f;
constexpr float kChromaRingLo = 0.2f;
constexpr float kMinGain      = 1e-6f;

auto RequireQueue(OpenClEncodeQueue& stream) -> cl_command_queue {
  if (stream.queue == nullptr) {
    throw std::runtime_error("OpenCL encode: product queue is null");
  }
  return stream.queue;
}

auto RequireMem(cl_mem native, const char* label) -> cl_mem {
  if (native == nullptr) {
    throw std::runtime_error(std::string("OpenCL encode: ") + label + " is null");
  }
  return native;
}

auto Kernel(const char* program, const char* name) -> cl_kernel {
  return OpenClKernelCache::Instance().GetKernel(program, name);
}

void Track(OpenClEncodeQueue& stream, cl_event event) {
  if (event != nullptr && stream.retain_event != nullptr) {
    stream.retain_event(event, stream.retain_ctx);
  } else if (event != nullptr) {
    clReleaseEvent(event);
  }
}

void Dispatch2D(OpenClEncodeQueue& stream, cl_kernel kernel, std::uint32_t width,
                std::uint32_t height) {
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(RequireQueue(stream), kernel, 2, nullptr, global, local, 0,
                                     nullptr, &event),
              "OpenCL encode enqueue");
  NoteOpenClEnqueueNdRange();
  Track(stream, event);
}

void SetMem(cl_kernel kernel, cl_uint index, cl_mem value, const char* what) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(cl_mem), &value), what);
}

void SetUInt(cl_kernel kernel, cl_uint index, std::uint32_t value, const char* what) {
  const cl_uint packed = value;
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(cl_uint), &packed), what);
}

auto ElementOffset(OpenClBufferView view, std::size_t element_bytes) -> std::uint32_t {
  if (element_bytes == 0 || (view.offset_bytes % element_bytes) != 0) {
    throw std::runtime_error("OpenCL encode: buffer offset is not aligned");
  }
  return view.offset_bytes / static_cast<std::uint32_t>(element_bytes);
}

auto ChannelRatio(float value, float green) -> float {
  if (!std::isfinite(value) || value <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(value / green, 0.25f, 4.0f);
}

auto InverseScale(const float* cam_mul) -> std::array<float, 3> {
  const float g = std::max(cam_mul[1], kMinGain);
  return {g / std::max(cam_mul[0], kMinGain), 1.0f, g / std::max(cam_mul[2], kMinGain)};
}

auto OrientedSize(std::uint32_t width, std::uint32_t height, int flip)
    -> std::pair<std::uint32_t, std::uint32_t> {
  if (flip == 5 || flip == 6) {
    return {height, width};
  }
  return {width, height};
}

auto MakeLinearizeParams(const RawLinearizationParams& linearization, const RawCfaPattern& pattern,
                         std::uint32_t width, std::uint32_t height)
    -> std::pair<WBParams, PatternParams> {
  WBParams wb{};
  for (int c = 0; c < 4; ++c) {
    wb.black_level[c]    = linearization.black_level[c];
    wb.white_level[c]    = linearization.white_level[c];
    wb.wb_multipliers[c] = linearization.cam_mul[c];
  }
  wb.apply_white_balance = linearization.apply_as_shot_wb != 0 ? 1 : 0;
  wb.black_tile_width    = linearization.black_tile_width;
  wb.black_tile_height   = linearization.black_tile_height;
  for (int i = 0; i < 36; ++i) {
    wb.pattern_black[i] = linearization.pattern_black[i];
  }

  PatternParams pattern_params{};
  pattern_params.width  = static_cast<int>(width);
  pattern_params.height = static_cast<int>(height);
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    pattern_params.tile_width  = 6;
    pattern_params.tile_height = 6;
    for (int i = 0; i < 36; ++i) {
      pattern_params.raw_fc[i] = pattern.xtrans_pattern.raw_fc[i];
    }
  } else {
    pattern_params.tile_width  = 2;
    pattern_params.tile_height = 2;
    for (int i = 0; i < 4; ++i) {
      pattern_params.raw_fc[i] = pattern.bayer_pattern.raw_fc[i];
    }
  }
  return {wb, pattern_params};
}

auto MakeHighlightParams(const float* cam_mul, std::uint32_t width, std::uint32_t height)
    -> HighlightParams {
  HighlightParams params{};
  const float     green = std::isfinite(cam_mul[1]) && cam_mul[1] > 0.0f ? cam_mul[1] : 1.0f;
  params.clips[0]       = kHilightMagic * ChannelRatio(cam_mul[0], green);
  params.clips[1]       = kHilightMagic;
  params.clips[2]       = kHilightMagic * ChannelRatio(cam_mul[2], green);
  params.clipdark[0]    = kChromaRingLo * params.clips[0];
  params.clipdark[1]    = kChromaRingLo * params.clips[1];
  params.clipdark[2]    = kChromaRingLo * params.clips[2];
  params.width          = width;
  params.height         = height;
  params.stride         = width;
  return params;
}

auto MakePackParams(RectI crop, std::uint32_t plane_width, std::uint32_t src_stride,
                    const float* cam_mul, int flip) -> PackOrientParams {
  const auto scale = InverseScale(cam_mul);
  const auto [dst_width, dst_height] =
      OrientedSize(static_cast<std::uint32_t>(crop.width), static_cast<std::uint32_t>(crop.height),
                   flip);
  PackOrientParams params{};
  params.src_x        = static_cast<std::uint32_t>(crop.x);
  params.src_y        = static_cast<std::uint32_t>(crop.y);
  params.src_width    = static_cast<std::uint32_t>(crop.width);
  params.src_height   = static_cast<std::uint32_t>(crop.height);
  params.dst_width    = dst_width;
  params.dst_height   = dst_height;
  params.flip         = static_cast<std::uint32_t>(flip);
  params.scale_r      = scale[0];
  params.scale_g      = scale[1];
  params.scale_b      = scale[2];
  params.plane_stride = plane_width;
  params.src_stride   = src_stride;
  return params;
}

}  // namespace

void EncodeLinearizeRgb(OpenClEncodeQueue& stream, OpenClBufferView rgba, std::uint32_t width,
                        std::uint32_t height, const RawRgbLinearizationParams& params) {
  auto kernel = Kernel(kCoreProgramName, RawProcessor::kLinearizeRgbKernelName);
  SetMem(kernel, 0, RequireMem(rgba.native, "RGB"), "RGB arg0");
  SetUInt(kernel, 1, width, "RGB arg1");
  SetUInt(kernel, 2, height, "RGB arg2");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(params), &params), "RGB params");
  SetUInt(kernel, 4, ElementOffset(rgba, 4 * sizeof(float)), "RGB offset");
  Dispatch2D(stream, kernel, width, height);
}

void EncodeToLinearRef(OpenClEncodeQueue& stream, OpenClBufferView src_u16,
                       OpenClBufferView dst_f32, std::uint32_t width, std::uint32_t height,
                       const RawLinearizationParams& linearization, const RawCfaPattern& pattern) {
  RequireMem(src_u16.native, "linearize source");
  RequireMem(dst_f32.native, "linearize destination");
  auto kernel              = Kernel(kCoreProgramName, kToLinearRefKernelName);
  const auto [wb, params]  = MakeLinearizeParams(linearization, pattern, width, height);
  SetMem(kernel, 0, src_u16.native, "linearize arg0");
  SetMem(kernel, 1, dst_f32.native, "linearize arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(wb), &wb), "linearize arg2");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(params), &params), "linearize arg3");
  SetUInt(kernel, 4, ElementOffset(src_u16, sizeof(std::uint16_t)), "linearize arg4");
  SetUInt(kernel, 5, ElementOffset(dst_f32, sizeof(float)), "linearize arg5");
  Dispatch2D(stream, kernel, width, height);
}

void EncodeCfaClamp01(OpenClEncodeQueue& stream, OpenClBufferView r32f, std::uint32_t width,
                      std::uint32_t height) {
  RequireMem(r32f.native, "cfa clamp");
  auto kernel = Kernel(kCoreProgramName, kCfaClamp01KernelName);
  SetMem(kernel, 0, r32f.native, "clamp arg0");
  SetUInt(kernel, 1, width, "clamp arg1");
  SetUInt(kernel, 2, height, "clamp arg2");
  SetUInt(kernel, 3, ElementOffset(r32f, sizeof(float)), "clamp arg3");
  Dispatch2D(stream, kernel, width, height);
}

void EncodeBayerRcd(OpenClEncodeQueue& stream, OpenClBufferView linear, OpenClBufferView r,
                    OpenClBufferView g, OpenClBufferView b, OpenClBufferView vh,
                    OpenClBufferView pq, const BayerPattern2x2& pattern, std::uint32_t width,
                    std::uint32_t height) {
  RequireMem(linear.native, "RCD CFA");
  RequireMem(r.native, "RCD R");
  RequireMem(g.native, "RCD G");
  RequireMem(b.native, "RCD B");
  RequireMem(vh.native, "RCD VH");
  RequireMem(pq.native, "RCD PQ");
  SinglePlaneParams params{
      .width  = width,
      .height = height,
      .stride = width,
      .rgb_fc = {static_cast<std::uint32_t>(pattern.rgb_fc[0]),
                 static_cast<std::uint32_t>(pattern.rgb_fc[1]),
                 static_cast<std::uint32_t>(pattern.rgb_fc[2]),
                 static_cast<std::uint32_t>(pattern.rgb_fc[3])},
  };
  const auto raw_off = ElementOffset(linear, sizeof(float));
  const auto r_off   = ElementOffset(r, sizeof(float));
  const auto g_off   = ElementOffset(g, sizeof(float));
  const auto b_off   = ElementOffset(b, sizeof(float));
  const auto vh_off  = ElementOffset(vh, sizeof(float));
  const auto pq_off  = ElementOffset(pq, sizeof(float));

  {
    auto kernel = Kernel(kDebayerRcdProgramName, kRcdInitAndVhKernelName);
    SetMem(kernel, 0, linear.native, "rcd0 arg0");
    SetMem(kernel, 1, r.native, "rcd0 arg1");
    SetMem(kernel, 2, g.native, "rcd0 arg2");
    SetMem(kernel, 3, b.native, "rcd0 arg3");
    SetMem(kernel, 4, vh.native, "rcd0 arg4");
    CheckOpenCl(clSetKernelArg(kernel, 5, sizeof(params), &params), "rcd0 arg5");
    SetUInt(kernel, 6, raw_off, "rcd0 arg6");
    SetUInt(kernel, 7, r_off, "rcd0 arg7");
    SetUInt(kernel, 8, g_off, "rcd0 arg8");
    SetUInt(kernel, 9, b_off, "rcd0 arg9");
    SetUInt(kernel, 10, vh_off, "rcd0 arg10");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kDebayerRcdProgramName, kRcdGreenAtRbKernelName);
    SetMem(kernel, 0, linear.native, "rcd1 arg0");
    SetMem(kernel, 1, vh.native, "rcd1 arg1");
    SetMem(kernel, 2, g.native, "rcd1 arg2");
    CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(params), &params), "rcd1 arg3");
    SetUInt(kernel, 4, raw_off, "rcd1 arg4");
    SetUInt(kernel, 5, vh_off, "rcd1 arg5");
    SetUInt(kernel, 6, g_off, "rcd1 arg6");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kDebayerRcdProgramName, kRcdPqDirKernelName);
    SetMem(kernel, 0, linear.native, "rcd2 arg0");
    SetMem(kernel, 1, pq.native, "rcd2 arg1");
    CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "rcd2 arg2");
    SetUInt(kernel, 3, raw_off, "rcd2 arg3");
    SetUInt(kernel, 4, pq_off, "rcd2 arg4");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kDebayerRcdProgramName, kRcdRbAtRbKernelName);
    SetMem(kernel, 0, pq.native, "rcd3 arg0");
    SetMem(kernel, 1, g.native, "rcd3 arg1");
    SetMem(kernel, 2, r.native, "rcd3 arg2");
    SetMem(kernel, 3, b.native, "rcd3 arg3");
    CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(params), &params), "rcd3 arg4");
    SetUInt(kernel, 5, pq_off, "rcd3 arg5");
    SetUInt(kernel, 6, g_off, "rcd3 arg6");
    SetUInt(kernel, 7, r_off, "rcd3 arg7");
    SetUInt(kernel, 8, b_off, "rcd3 arg8");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kDebayerRcdProgramName, kRcdRbAtGKernelName);
    SetMem(kernel, 0, vh.native, "rcd4 arg0");
    SetMem(kernel, 1, g.native, "rcd4 arg1");
    SetMem(kernel, 2, r.native, "rcd4 arg2");
    SetMem(kernel, 3, b.native, "rcd4 arg3");
    CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(params), &params), "rcd4 arg4");
    SetUInt(kernel, 5, vh_off, "rcd4 arg5");
    SetUInt(kernel, 6, g_off, "rcd4 arg6");
    SetUInt(kernel, 7, r_off, "rcd4 arg7");
    SetUInt(kernel, 8, b_off, "rcd4 arg8");
    Dispatch2D(stream, kernel, width, height);
  }
}

void EncodeXTrans(OpenClEncodeQueue& stream, OpenClBufferView linear, OpenClBufferView green,
                  OpenClBufferView rgba, const XTransPattern6x6& pattern, std::uint32_t width,
                  std::uint32_t height, int passes) {
  RequireMem(linear.native, "X-Trans CFA");
  RequireMem(green.native, "X-Trans green");
  RequireMem(rgba.native, "X-Trans RGBA");
  XTransParams params{};
  params.width        = width;
  params.height       = height;
  params.tile_width   = 6;
  params.tile_height  = 6;
  params.passes       = static_cast<std::uint32_t>(std::max(passes, 1));
  params.green_radius = 3;
  params.rb_radius    = params.passes > 1 ? 4U : 3U;
  for (int i = 0; i < 36; ++i) {
    params.rgb_fc[i] = static_cast<std::uint32_t>(pattern.rgb_fc[i]);
  }
  const auto raw_off   = ElementOffset(linear, sizeof(float));
  const auto green_off = ElementOffset(green, sizeof(float));
  const auto rgba_off  = ElementOffset(rgba, sizeof(float) * 4);
  {
    auto kernel = Kernel(kXTransProgramName, kXTransGreenKernelName);
    SetMem(kernel, 0, linear.native, "xtrans green arg0");
    SetMem(kernel, 1, green.native, "xtrans green arg1");
    CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "xtrans green arg2");
    SetUInt(kernel, 3, raw_off, "xtrans green arg3");
    SetUInt(kernel, 4, green_off, "xtrans green arg4");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kXTransProgramName, kXTransRgbaKernelName);
    SetMem(kernel, 0, linear.native, "xtrans rgba arg0");
    SetMem(kernel, 1, green.native, "xtrans rgba arg1");
    SetMem(kernel, 2, rgba.native, "xtrans rgba arg2");
    CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(params), &params), "xtrans rgba arg3");
    SetUInt(kernel, 4, raw_off, "xtrans rgba arg4");
    SetUInt(kernel, 5, green_off, "xtrans rgba arg5");
    SetUInt(kernel, 6, rgba_off, "xtrans rgba arg6");
    Dispatch2D(stream, kernel, width, height);
  }
}

void EncodeHighlightReconstruct(OpenClEncodeQueue& stream, OpenClBufferView src_rgba,
                                OpenClBufferView dst_rgba, OpenClBufferView mask,
                                OpenClBufferView dilated_mask, OpenClBufferView sums,
                                OpenClBufferView cnts, OpenClBufferView anyclipped,
                                const float* cam_mul, std::uint32_t width, std::uint32_t height) {
  RequireMem(src_rgba.native, "HLR source");
  RequireMem(dst_rgba.native, "HLR destination");
  RequireMem(mask.native, "HLR mask");
  RequireMem(dilated_mask.native, "HLR dilated mask");
  RequireMem(sums.native, "HLR sums");
  RequireMem(cnts.native, "HLR counts");
  RequireMem(anyclipped.native, "HLR anyclipped");
  const auto params     = MakeHighlightParams(cam_mul, width, height);
  const auto in_off     = ElementOffset(src_rgba, sizeof(float) * 4);
  const auto out_off    = ElementOffset(dst_rgba, sizeof(float) * 4);
  const auto mask_off   = ElementOffset(mask, sizeof(std::uint8_t));
  const auto dilate_off = ElementOffset(dilated_mask, sizeof(std::uint8_t));
  const auto sums_off   = ElementOffset(sums, sizeof(float));
  const auto cnts_off   = ElementOffset(cnts, sizeof(float));
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrBuildMaskKernelName);
    SetMem(kernel, 0, src_rgba.native, "hlr mask arg0");
    SetMem(kernel, 1, mask.native, "hlr mask arg1");
    SetMem(kernel, 2, anyclipped.native, "hlr mask arg2");
    CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(params), &params), "hlr mask arg3");
    SetUInt(kernel, 4, in_off, "hlr mask arg4");
    SetUInt(kernel, 5, mask_off, "hlr mask arg5");
    SetUInt(kernel, 6, ElementOffset(anyclipped, sizeof(cl_int)), "hlr mask arg6");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrDilateMaskKernelName);
    SetMem(kernel, 0, mask.native, "hlr dilate arg0");
    SetMem(kernel, 1, dilated_mask.native, "hlr dilate arg1");
    CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "hlr dilate arg2");
    SetUInt(kernel, 3, mask_off, "hlr dilate arg3");
    SetUInt(kernel, 4, dilate_off, "hlr dilate arg4");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrChrominanceContribKernelName);
    SetMem(kernel, 0, src_rgba.native, "hlr chroma arg0");
    SetMem(kernel, 1, dilated_mask.native, "hlr chroma arg1");
    SetMem(kernel, 2, sums.native, "hlr chroma arg2");
    SetMem(kernel, 3, cnts.native, "hlr chroma arg3");
    CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(params), &params), "hlr chroma arg4");
    SetUInt(kernel, 5, in_off, "hlr chroma arg5");
    SetUInt(kernel, 6, dilate_off, "hlr chroma arg6");
    SetUInt(kernel, 7, sums_off, "hlr chroma arg7");
    SetUInt(kernel, 8, cnts_off, "hlr chroma arg8");
    Dispatch2D(stream, kernel, width, height);
  }
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrReconstructFromStatsKernelName);
    SetMem(kernel, 0, src_rgba.native, "hlr recon arg0");
    SetMem(kernel, 1, dst_rgba.native, "hlr recon arg1");
    SetMem(kernel, 2, sums.native, "hlr recon arg2");
    SetMem(kernel, 3, cnts.native, "hlr recon arg3");
    CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(params), &params), "hlr recon arg4");
    SetUInt(kernel, 5, in_off, "hlr recon arg5");
    SetUInt(kernel, 6, out_off, "hlr recon arg6");
    SetUInt(kernel, 7, sums_off, "hlr recon arg7");
    SetUInt(kernel, 8, cnts_off, "hlr recon arg8");
    Dispatch2D(stream, kernel, width, height);
  }
}

void EncodeHighlightReconstructPlanarAndPack(
    OpenClEncodeQueue& stream, OpenClBufferView r, OpenClBufferView g, OpenClBufferView b,
    cl_mem dst_rgba, OpenClBufferView mask, OpenClBufferView dilated_mask, OpenClBufferView sums,
    OpenClBufferView cnts, OpenClBufferView anyclipped, const float* cam_mul, RectI crop,
    std::uint32_t plane_width, int flip) {
  RequireMem(r.native, "HLR R");
  RequireMem(g.native, "HLR G");
  RequireMem(b.native, "HLR B");
  RequireMem(dst_rgba, "HLR destination");
  RequireMem(mask.native, "HLR mask");
  RequireMem(dilated_mask.native, "HLR dilated mask");
  RequireMem(sums.native, "HLR sums");
  RequireMem(cnts.native, "HLR counts");
  RequireMem(anyclipped.native, "HLR anyclipped");
  const auto crop_w    = static_cast<std::uint32_t>(crop.width);
  const auto crop_h    = static_cast<std::uint32_t>(crop.height);
  auto       params     = MakeHighlightParams(cam_mul, crop_w, crop_h);
  params.stride         = plane_width;
  const auto pack       = MakePackParams(crop, plane_width, plane_width, cam_mul, flip);
  const auto r_off     = ElementOffset(r, sizeof(float));
  const auto g_off     = ElementOffset(g, sizeof(float));
  const auto b_off     = ElementOffset(b, sizeof(float));
  const auto mask_off   = ElementOffset(mask, sizeof(std::uint8_t));
  const auto dilate_off = ElementOffset(dilated_mask, sizeof(std::uint8_t));
  const auto sums_off   = ElementOffset(sums, sizeof(float));
  const auto cnts_off   = ElementOffset(cnts, sizeof(float));
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrBuildMaskPlanarKernelName);
    SetMem(kernel, 0, r.native, "hlr planar mask arg0");
    SetMem(kernel, 1, g.native, "hlr planar mask arg1");
    SetMem(kernel, 2, b.native, "hlr planar mask arg2");
    SetMem(kernel, 3, mask.native, "hlr planar mask arg3");
    SetMem(kernel, 4, anyclipped.native, "hlr planar mask arg4");
    CheckOpenCl(clSetKernelArg(kernel, 5, sizeof(params), &params), "hlr planar mask arg5");
    SetUInt(kernel, 6, r_off, "hlr planar mask arg6");
    SetUInt(kernel, 7, g_off, "hlr planar mask arg7");
    SetUInt(kernel, 8, b_off, "hlr planar mask arg8");
    SetUInt(kernel, 9, mask_off, "hlr planar mask arg9");
    SetUInt(kernel, 10, ElementOffset(anyclipped, sizeof(cl_int)), "hlr planar mask arg10");
    SetUInt(kernel, 11, pack.src_x, "hlr planar mask arg11");
    SetUInt(kernel, 12, pack.src_y, "hlr planar mask arg12");
    Dispatch2D(stream, kernel, crop_w, crop_h);
  }
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrDilateMaskKernelName);
    SetMem(kernel, 0, mask.native, "hlr planar dilate arg0");
    SetMem(kernel, 1, dilated_mask.native, "hlr planar dilate arg1");
    CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "hlr planar dilate arg2");
    SetUInt(kernel, 3, mask_off, "hlr planar dilate arg3");
    SetUInt(kernel, 4, dilate_off, "hlr planar dilate arg4");
    Dispatch2D(stream, kernel, crop_w, crop_h);
  }
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrChrominanceContribPlanarKernelName);
    SetMem(kernel, 0, r.native, "hlr planar chroma arg0");
    SetMem(kernel, 1, g.native, "hlr planar chroma arg1");
    SetMem(kernel, 2, b.native, "hlr planar chroma arg2");
    SetMem(kernel, 3, dilated_mask.native, "hlr planar chroma arg3");
    SetMem(kernel, 4, sums.native, "hlr planar chroma arg4");
    SetMem(kernel, 5, cnts.native, "hlr planar chroma arg5");
    CheckOpenCl(clSetKernelArg(kernel, 6, sizeof(params), &params), "hlr planar chroma arg6");
    SetUInt(kernel, 7, r_off, "hlr planar chroma arg7");
    SetUInt(kernel, 8, g_off, "hlr planar chroma arg8");
    SetUInt(kernel, 9, b_off, "hlr planar chroma arg9");
    SetUInt(kernel, 10, dilate_off, "hlr planar chroma arg10");
    SetUInt(kernel, 11, sums_off, "hlr planar chroma arg11");
    SetUInt(kernel, 12, cnts_off, "hlr planar chroma arg12");
    SetUInt(kernel, 13, pack.src_x, "hlr planar chroma arg13");
    SetUInt(kernel, 14, pack.src_y, "hlr planar chroma arg14");
    Dispatch2D(stream, kernel, crop_w, crop_h);
  }
  {
    auto kernel = Kernel(kHighlightProgramName, kHlrReconstructFromStatsPlanarPackKernelName);
    SetMem(kernel, 0, r.native, "hlr planar pack arg0");
    SetMem(kernel, 1, g.native, "hlr planar pack arg1");
    SetMem(kernel, 2, b.native, "hlr planar pack arg2");
    SetMem(kernel, 3, dst_rgba, "hlr planar pack arg3");
    SetMem(kernel, 4, sums.native, "hlr planar pack arg4");
    SetMem(kernel, 5, cnts.native, "hlr planar pack arg5");
    CheckOpenCl(clSetKernelArg(kernel, 6, sizeof(params), &params), "hlr planar pack arg6");
    CheckOpenCl(clSetKernelArg(kernel, 7, sizeof(pack), &pack), "hlr planar pack arg7");
    SetUInt(kernel, 8, r_off, "hlr planar pack arg8");
    SetUInt(kernel, 9, g_off, "hlr planar pack arg9");
    SetUInt(kernel, 10, b_off, "hlr planar pack arg10");
    SetUInt(kernel, 11, sums_off, "hlr planar pack arg11");
    SetUInt(kernel, 12, cnts_off, "hlr planar pack arg12");
    Dispatch2D(stream, kernel, crop_w, crop_h);
  }
}

void EncodePackPlanesCropInverseOrient(OpenClEncodeQueue& stream, OpenClBufferView r,
                                       OpenClBufferView g, OpenClBufferView b, cl_mem dst_rgba,
                                       RectI crop, std::uint32_t plane_width, const float* cam_mul,
                                       int flip) {
  RequireMem(r.native, "pack R");
  RequireMem(g.native, "pack G");
  RequireMem(b.native, "pack B");
  RequireMem(dst_rgba, "pack destination");
  const auto params = MakePackParams(crop, plane_width, plane_width, cam_mul, flip);
  auto       kernel = Kernel(kCvtRefSpaceProgramName, kPackPlanesCropInverseOrientKernelName);
  SetMem(kernel, 0, r.native, "pack arg0");
  SetMem(kernel, 1, g.native, "pack arg1");
  SetMem(kernel, 2, b.native, "pack arg2");
  SetMem(kernel, 3, dst_rgba, "pack arg3");
  CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(params), &params), "pack arg4");
  SetUInt(kernel, 5, ElementOffset(r, sizeof(float)), "pack arg5");
  SetUInt(kernel, 6, ElementOffset(g, sizeof(float)), "pack arg6");
  SetUInt(kernel, 7, ElementOffset(b, sizeof(float)), "pack arg7");
  Dispatch2D(stream, kernel, params.src_width, params.src_height);
}

void EncodeCopyRgbaCropInverseOrient(OpenClEncodeQueue& stream, OpenClBufferView src_rgba,
                                     cl_mem dst_rgba, RectI crop, std::uint32_t src_width,
                                     const float* cam_mul, int flip) {
  RequireMem(src_rgba.native, "copy source");
  RequireMem(dst_rgba, "copy destination");
  const auto params = MakePackParams(crop, src_width, src_width, cam_mul, flip);
  auto       kernel = Kernel(kCvtRefSpaceProgramName, kCopyRgbaCropInverseOrientKernelName);
  SetMem(kernel, 0, src_rgba.native, "copy arg0");
  SetMem(kernel, 1, dst_rgba, "copy arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "copy arg2");
  SetUInt(kernel, 3, ElementOffset(src_rgba, sizeof(float) * 4), "copy arg3");
  Dispatch2D(stream, kernel, params.src_width, params.src_height);
}

void EncodeCopyRgbCropInverseOrient(OpenClEncodeQueue& stream, OpenClBufferView src_rgb,
                                     cl_mem dst_rgba, RectI crop, std::uint32_t src_width,
                                     const float* cam_mul, int flip) {
  RequireMem(src_rgb.native, "copy rgb source");
  RequireMem(dst_rgba, "copy rgb destination");
  const auto params = MakePackParams(crop, src_width, src_width, cam_mul, flip);
  auto       kernel = Kernel(kCvtRefSpaceProgramName, kCopyRgbCropInverseOrientKernelName);
  SetMem(kernel, 0, src_rgb.native, "copy rgb arg0");
  SetMem(kernel, 1, dst_rgba, "copy rgb arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "copy rgb arg2");
  SetUInt(kernel, 3, ElementOffset(src_rgb, sizeof(float)), "copy rgb arg3");
  Dispatch2D(stream, kernel, params.src_width, params.src_height);
}

}  // namespace alcedo::OpenCL

#endif
