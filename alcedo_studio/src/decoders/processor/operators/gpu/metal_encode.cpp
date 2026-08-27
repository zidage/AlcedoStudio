//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_encode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include <alcedo/metal/Metal.hpp>

#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_utils/geometry_utils.hpp"

namespace alcedo::metal {
namespace {

struct WBParams {
  float    black_level[4];
  float    white_level[4];
  float    wb_multipliers[4];
  uint32_t apply_white_balance;
  uint32_t padding[3];
};

struct ToLinearRefParams {
  uint32_t width;
  uint32_t height;
  uint32_t tile_width;
  uint32_t tile_height;
  uint32_t black_tile_width;
  uint32_t black_tile_height;
  uint32_t raw_fc[36];
};

struct ClampParams {
  uint32_t width;
  uint32_t height;
};

struct SinglePlaneParams {
  uint32_t width;
  uint32_t height;
  uint32_t rgb_fc[4];
};

struct XTransParams {
  uint32_t width;
  uint32_t height;
  uint32_t tile_width;
  uint32_t tile_height;
  uint32_t passes;
  uint32_t green_radius;
  uint32_t rb_radius;
  uint32_t rgb_fc[36];
};

struct HighlightParams {
  float    clips[4];
  float    clipdark[4];
  uint32_t width;
  uint32_t height;
};

struct PackOrientParams {
  uint32_t src_x;
  uint32_t src_y;
  uint32_t src_width;
  uint32_t src_height;
  uint32_t dst_width;
  uint32_t dst_height;
  uint32_t flip;
  float    scale_r;
  float    scale_g;
  float    scale_b;
};

constexpr float kHilightMagic = 0.987f;
constexpr float kChromaRingLo = 0.2f;
constexpr float kMinGain      = 1e-6f;

auto CommandBuffer(void* command_buffer) -> MTL::CommandBuffer* {
  auto* buffer = static_cast<MTL::CommandBuffer*>(command_buffer);
  if (buffer == nullptr) {
    throw std::runtime_error("Metal encode: command buffer is null");
  }
  return buffer;
}

auto Texture(void* texture, const char* label) -> MTL::Texture* {
  auto* native = static_cast<MTL::Texture*>(texture);
  if (native == nullptr) {
    throw std::runtime_error(std::string("Metal encode: ") + label + " texture is null");
  }
  return native;
}

auto Pipeline(const char* metallib_path, const char* function_name, const char* label)
    -> NS::SharedPtr<MTL::ComputePipelineState> {
  if (metallib_path == nullptr || metallib_path[0] == '\0') {
    throw std::runtime_error(std::string(label) + ": metallib path is not configured.");
  }
  return ComputePipelineCache::Instance().GetPipelineState(metallib_path, function_name, label);
}

void Dispatch(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
              uint32_t width, uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{width, height, 1},
                           MTL::Size{thread_width, thread_height, 1});
}

auto Encoder(MTL::CommandBuffer* buffer) -> NS::SharedPtr<MTL::ComputeCommandEncoder> {
  auto encoder = NS::RetainPtr(buffer->computeCommandEncoder());
  if (!encoder) {
    throw std::runtime_error("Metal encode: failed to create compute encoder");
  }
  return encoder;
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

auto OrientedSize(uint32_t width, uint32_t height, int flip) -> std::pair<uint32_t, uint32_t> {
  if (flip == 5 || flip == 6) {
    return {height, width};
  }
  return {width, height};
}

}  // namespace

void EncodeToLinearRef(void* command_buffer, void* src_r16u, void* dst_r32f,
                       const RawLinearizationParams& linearization, const RawCfaPattern& pattern) {
  auto* buffer = CommandBuffer(command_buffer);
  auto* src    = Texture(src_r16u, "linearize source");
  auto* dst    = Texture(dst_r32f, "linearize destination");
#ifndef ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH
  throw std::runtime_error("Metal ToLinearRef metallib path is not configured.");
#else
  auto pipeline = Pipeline(ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH, "to_linear_ref_r16u",
                           "Metal ToLinearRef");
#endif
  ToLinearRefParams params = {};
  params.width             = static_cast<uint32_t>(src->width());
  params.height            = static_cast<uint32_t>(src->height());
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    params.tile_width  = 6;
    params.tile_height = 6;
    for (int i = 0; i < 36; ++i) {
      params.raw_fc[i] = static_cast<uint32_t>(pattern.xtrans_pattern.raw_fc[i]);
    }
  } else {
    params.tile_width  = 2;
    params.tile_height = 2;
    for (int i = 0; i < 4; ++i) {
      params.raw_fc[i] = static_cast<uint32_t>(pattern.bayer_pattern.raw_fc[i]);
    }
  }
  params.black_tile_width  = static_cast<uint32_t>(std::max(linearization.black_tile_width, 0));
  params.black_tile_height = static_cast<uint32_t>(std::max(linearization.black_tile_height, 0));

  WBParams wb = {};
  for (int c = 0; c < 4; ++c) {
    wb.black_level[c]    = linearization.black_level[c];
    wb.white_level[c]    = linearization.white_level[c];
    wb.wb_multipliers[c] = linearization.cam_mul[c];
  }
  wb.apply_white_balance = linearization.apply_as_shot_wb != 0 ? 1U : 0U;

  auto compute = Encoder(buffer);
  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(src, 0);
  compute->setTexture(dst, 1);
  compute->setBytes(&params, sizeof(params), 0);
  compute->setBytes(&wb, sizeof(wb), 1);
  compute->setBytes(linearization.pattern_black, sizeof(linearization.pattern_black), 2);
  Dispatch(compute.get(), pipeline.get(), params.width, params.height);
  compute->endEncoding();
}

void EncodeCfaClamp01(void* command_buffer, void* r32f, std::uint32_t width,
                      std::uint32_t height) {
  auto* buffer = CommandBuffer(command_buffer);
  auto* image  = Texture(r32f, "cfa clamp");
#ifndef ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH
  throw std::runtime_error("Metal ToLinearRef metallib path is not configured.");
#else
  auto pipeline =
      Pipeline(ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH, "cfa_clamp01_r32f", "Metal CFA Clamp01");
#endif
  const ClampParams params{width, height};
  auto              compute = Encoder(buffer);
  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(image, 0);
  compute->setBytes(&params, sizeof(params), 0);
  Dispatch(compute.get(), pipeline.get(), width, height);
  compute->endEncoding();
}

void EncodeBayerRcd(void* command_buffer, void* linear_cfa, void* r, void* g, void* b, void* vh,
                    void* pq, const BayerPattern2x2& pattern, std::uint32_t width,
                    std::uint32_t height) {
  auto* buffer = CommandBuffer(command_buffer);
  auto* raw    = Texture(linear_cfa, "RCD CFA");
  auto* red    = Texture(r, "RCD R");
  auto* green  = Texture(g, "RCD G");
  auto* blue   = Texture(b, "RCD B");
  auto* vh_tex = Texture(vh, "RCD VH");
  auto* pq_tex = Texture(pq, "RCD PQ");
#ifndef ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH
  throw std::runtime_error("Metal Debayer RCD metallib path is not configured.");
#else
  const char* lib = ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH;
#endif
  SinglePlaneParams params{
      .width  = width,
      .height = height,
      .rgb_fc = {static_cast<uint32_t>(pattern.rgb_fc[0]), static_cast<uint32_t>(pattern.rgb_fc[1]),
                 static_cast<uint32_t>(pattern.rgb_fc[2]), static_cast<uint32_t>(pattern.rgb_fc[3])},
  };

  auto dispatch = [&](const char* name, auto bind) {
    auto pipeline = Pipeline(lib, name, "Metal Debayer RCD");
    auto compute  = Encoder(buffer);
    compute->setComputePipelineState(pipeline.get());
    bind(compute.get());
    compute->setBytes(&params, sizeof(params), 0);
    Dispatch(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  };
  dispatch("rcd_init_and_vh", [&](MTL::ComputeCommandEncoder* compute) {
    compute->setTexture(raw, 0);
    compute->setTexture(red, 1);
    compute->setTexture(green, 2);
    compute->setTexture(blue, 3);
    compute->setTexture(vh_tex, 4);
  });
  dispatch("rcd_green_at_rb", [&](MTL::ComputeCommandEncoder* compute) {
    compute->setTexture(raw, 0);
    compute->setTexture(vh_tex, 1);
    compute->setTexture(green, 2);
  });
  dispatch("rcd_pq_dir", [&](MTL::ComputeCommandEncoder* compute) {
    compute->setTexture(raw, 0);
    compute->setTexture(pq_tex, 1);
  });
  dispatch("rcd_rb_at_rb", [&](MTL::ComputeCommandEncoder* compute) {
    compute->setTexture(pq_tex, 0);
    compute->setTexture(green, 1);
    compute->setTexture(red, 2);
    compute->setTexture(blue, 3);
  });
  dispatch("rcd_rb_at_g", [&](MTL::ComputeCommandEncoder* compute) {
    compute->setTexture(vh_tex, 0);
    compute->setTexture(green, 1);
    compute->setTexture(red, 2);
    compute->setTexture(blue, 3);
  });
}

void EncodeXTrans(void* command_buffer, void* linear_cfa, void* green, void* rgba,
                  const XTransPattern6x6& pattern, std::uint32_t width, std::uint32_t height,
                  int passes) {
  auto* buffer     = CommandBuffer(command_buffer);
  auto* raw        = Texture(linear_cfa, "X-Trans CFA");
  auto* green_tex  = Texture(green, "X-Trans green");
  auto* output     = Texture(rgba, "X-Trans RGBA");
#ifndef ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH
  throw std::runtime_error("Metal X-Trans interpolate metallib path is not configured.");
#else
  const char* lib = ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH;
#endif
  XTransParams params = {};
  params.width        = width;
  params.height       = height;
  params.tile_width   = 6;
  params.tile_height  = 6;
  params.passes       = static_cast<uint32_t>(std::max(passes, 1));
  params.green_radius = 3;
  params.rb_radius    = params.passes > 1 ? 4U : 3U;
  for (int i = 0; i < 36; ++i) {
    params.rgb_fc[i] = static_cast<uint32_t>(pattern.rgb_fc[i]);
  }
  {
    auto pipeline = Pipeline(lib, "xtrans_green", "Metal X-Trans interpolate");
    auto compute  = Encoder(buffer);
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(raw, 0);
    compute->setTexture(green_tex, 1);
    compute->setBytes(&params, sizeof(params), 0);
    Dispatch(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }
  {
    auto pipeline = Pipeline(lib, "xtrans_rgba", "Metal X-Trans interpolate");
    auto compute  = Encoder(buffer);
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(raw, 0);
    compute->setTexture(green_tex, 1);
    compute->setTexture(output, 2);
    compute->setBytes(&params, sizeof(params), 0);
    Dispatch(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }
}

void EncodeHighlightReconstruct(void* command_buffer, void* src_rgba, void* dst_rgba,
                                void* stats_buffer, std::uint32_t stats_offset, const float* cam_mul,
                                std::uint32_t width, std::uint32_t height) {
  auto* buffer = CommandBuffer(command_buffer);
  auto* src    = Texture(src_rgba, "HLR source");
  auto* dst    = Texture(dst_rgba, "HLR destination");
  auto* stats  = static_cast<MTL::Buffer*>(stats_buffer);
  if (stats == nullptr) {
    throw std::runtime_error("Metal encode: HLR stats buffer is null");
  }
#ifndef ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH
  throw std::runtime_error("Metal HighlightReconstruct metallib path is not configured.");
#else
  const char* lib = ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH;
#endif
  HighlightParams params = {};
  const float     green  = std::isfinite(cam_mul[1]) && cam_mul[1] > 0.0f ? cam_mul[1] : 1.0f;
  params.clips[0]        = kHilightMagic * ChannelRatio(cam_mul[0], green);
  params.clips[1]        = kHilightMagic;
  params.clips[2]        = kHilightMagic * ChannelRatio(cam_mul[2], green);
  params.clipdark[0]     = kChromaRingLo * params.clips[0];
  params.clipdark[1]     = kChromaRingLo * params.clips[1];
  params.clipdark[2]     = kChromaRingLo * params.clips[2];
  params.width           = width;
  params.height          = height;
  {
    auto pipeline = Pipeline(lib, "hlr_accumulate_stats", "Metal HighlightReconstruct");
    auto compute  = Encoder(buffer);
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(src, 0);
    compute->setBuffer(stats, stats_offset, 0);
    compute->setBytes(&params, sizeof(params), 1);
    Dispatch(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }
  {
    auto pipeline = Pipeline(lib, "hlr_reconstruct_tex", "Metal HighlightReconstruct");
    auto compute  = Encoder(buffer);
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(src, 0);
    compute->setTexture(dst, 1);
    compute->setBuffer(stats, stats_offset, 0);
    compute->setBytes(&params, sizeof(params), 1);
    Dispatch(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }
}

void EncodePackPlanesCropInverseOrient(void* command_buffer, void* r, void* g, void* b,
                                       void* dst_rgba, RectI crop, const float* cam_mul, int flip) {
  auto* buffer = CommandBuffer(command_buffer);
  auto* red    = Texture(r, "pack R");
  auto* green  = Texture(g, "pack G");
  auto* blue   = Texture(b, "pack B");
  auto* dst    = Texture(dst_rgba, "pack destination");
#ifndef ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH
  throw std::runtime_error("Metal ApplyInverseCamMul metallib path is not configured.");
#else
  auto pipeline = Pipeline(ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH,
                           "pack_planes_crop_inverse_orient", "Metal pack");
#endif
  const auto scale                    = InverseScale(cam_mul);
  const auto [dst_width, dst_height]  = OrientedSize(static_cast<uint32_t>(crop.width),
                                                    static_cast<uint32_t>(crop.height), flip);
  PackOrientParams params{
      .src_x      = static_cast<uint32_t>(crop.x),
      .src_y      = static_cast<uint32_t>(crop.y),
      .src_width  = static_cast<uint32_t>(crop.width),
      .src_height = static_cast<uint32_t>(crop.height),
      .dst_width  = dst_width,
      .dst_height = dst_height,
      .flip       = static_cast<uint32_t>(flip),
      .scale_r    = scale[0],
      .scale_g    = scale[1],
      .scale_b    = scale[2],
  };
  auto compute = Encoder(buffer);
  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(red, 0);
  compute->setTexture(green, 1);
  compute->setTexture(blue, 2);
  compute->setTexture(dst, 3);
  compute->setBytes(&params, sizeof(params), 0);
  Dispatch(compute.get(), pipeline.get(), params.src_width, params.src_height);
  compute->endEncoding();
}

void EncodeCopyRgbaCropInverseOrient(void* command_buffer, void* src_rgba, void* dst_rgba,
                                     RectI crop, const float* cam_mul, int flip) {
  auto* buffer = CommandBuffer(command_buffer);
  auto* src    = Texture(src_rgba, "copy source");
  auto* dst    = Texture(dst_rgba, "copy destination");
#ifndef ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH
  throw std::runtime_error("Metal ApplyInverseCamMul metallib path is not configured.");
#else
  auto pipeline = Pipeline(ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH, "copy_rgba_crop_inverse_orient",
                           "Metal pack");
#endif
  const auto scale                   = InverseScale(cam_mul);
  const auto [dst_width, dst_height] = OrientedSize(static_cast<uint32_t>(crop.width),
                                                    static_cast<uint32_t>(crop.height), flip);
  PackOrientParams params{
      .src_x      = static_cast<uint32_t>(crop.x),
      .src_y      = static_cast<uint32_t>(crop.y),
      .src_width  = static_cast<uint32_t>(crop.width),
      .src_height = static_cast<uint32_t>(crop.height),
      .dst_width  = dst_width,
      .dst_height = dst_height,
      .flip       = static_cast<uint32_t>(flip),
      .scale_r    = scale[0],
      .scale_g    = scale[1],
      .scale_b    = scale[2],
  };
  auto compute = Encoder(buffer);
  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(src, 0);
  compute->setTexture(dst, 1);
  compute->setBytes(&params, sizeof(params), 0);
  Dispatch(compute.get(), pipeline.get(), params.src_width, params.src_height);
  compute->endEncoding();
}

void EncodeWarpRectilinear(void* command_buffer, void* src_rgba, void* dst_rgba,
                           const dng::WarpRectilinear& warp, std::uint32_t, std::uint32_t) {
  utils::EncodeWarpRectilinearTexture(command_buffer, src_rgba, dst_rgba, warp);
}

}  // namespace alcedo::metal

#endif
