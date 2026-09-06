//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/opencl/opencl_develop_pass.hpp"
#include "edit/runtime/opencl/opencl_drt_pass.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#include "edit/runtime/opencl/opencl_primary_grade_pass.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "gpu/transient_buffer_arena.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto MakeNeighborhoodPlane(std::uint32_t width, std::uint32_t height, float surroundings,
                           float center) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* pointer) { delete[] pointer; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float value = x == width / 2 && y == height / 2 ? center : surroundings;
      const auto  index = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = value;
      pixels[index + 1] = value;
      pixels[index + 2] = value;
      pixels[index + 3] = 1.0f;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

auto Download(OpenClRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       texture = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto Luma(const Rgba& c) -> float { return 0.272229f * c.r + 0.674082f * c.g + 0.053689f * c.b; }

auto ExtrapolateCurve(float value, const GradeAdjustmentParams& p, std::uint32_t a, std::uint32_t b)
    -> float {
  const float x0 = p.values[a * 2];
  const float y0 = p.values[a * 2 + 1];
  const float x1 = p.values[b * 2];
  const float y1 = p.values[b * 2 + 1];
  return y0 + (value - x0) * (y1 - y0) / std::max(x1 - x0, 1.0e-6f);
}

auto ApplyCurve(float value, const GradeAdjustmentParams& p) -> float {
  if (p.count < 2) {
    return value;
  }
  if (value <= p.values[0]) {
    return ExtrapolateCurve(value, p, 0, 1);
  }
  for (std::uint32_t i = 1; i < p.count; ++i) {
    const float x1 = p.values[i * 2];
    if (value <= x1) {
      const float x0 = p.values[(i - 1) * 2];
      const float y0 = p.values[(i - 1) * 2 + 1];
      const float y1 = p.values[i * 2 + 1];
      const float t  = (value - x0) / std::max(x1 - x0, 1.0e-6f);
      return y0 + t * (y1 - y0);
    }
  }
  return ExtrapolateCurve(value, p, p.count - 2, p.count - 1);
}

auto ApplyHls(Rgba c, const GradeAdjustmentParams& p) -> Rgba {
  const float maximum = std::max(c.r, std::max(c.g, c.b));
  const float minimum = std::min(c.r, std::min(c.g, c.b));
  const float chroma  = maximum - minimum;
  float       hue     = 0.0f;
  if (chroma > 1.0e-6f) {
    if (maximum == c.r) {
      hue = 60.0f * std::fmod((c.g - c.b) / chroma, 6.0f);
    } else if (maximum == c.g) {
      hue = 60.0f * ((c.b - c.r) / chroma + 2.0f);
    } else {
      hue = 60.0f * ((c.r - c.g) / chroma + 4.0f);
    }
  }
  if (hue < 0.0f) {
    hue += 360.0f;
  }
  const int   bin        = static_cast<int>((hue + 22.5f) / 45.0f) & 7;
  const float luma       = Luma(c);
  const float saturation = 1.0f + p.values[16 + bin];
  c.r                    = luma + (c.r - luma) * saturation;
  c.g                    = luma + (c.g - luma) * saturation;
  c.b                    = luma + (c.b - luma) * saturation;
  const float lightness  = p.values[8 + bin];
  c.r += lightness;
  c.g += lightness;
  c.b += lightness;
  return c;
}

auto CpuApplyAdjustment(Rgba c, const GradeAdjustmentParams& p) -> Rgba {
  const auto  behavior = static_cast<AdjustmentBehavior>(p.behavior);
  const float value    = p.values[0];
  if (behavior == AdjustmentBehavior::Cat02WhiteBalance && value != 0.0f) {
    const float temperature = p.values[1] * 0.001f;
    const float tint        = p.values[2] * 0.001f;
    c.r *= std::exp2(temperature - tint * 0.5f);
    c.g *= std::exp2(tint);
    c.b *= std::exp2(-temperature - tint * 0.5f);
  } else if (behavior == AdjustmentBehavior::Exposure) {
    const float offset = value / 17.52f;
    c.r += offset;
    c.g += offset;
    c.b += offset;
  } else if (behavior == AdjustmentBehavior::Contrast) {
    const float scale = 1.0f + value * 0.01f;
    c.r               = (c.r - 0.18f) * scale + 0.18f;
    c.g               = (c.g - 0.18f) * scale + 0.18f;
    c.b               = (c.b - 0.18f) * scale + 0.18f;
  } else if (behavior == AdjustmentBehavior::White) {
    const float gain = 1.0f + std::max(value, 0.0f) * 0.005f;
    c.r *= gain;
    c.g *= gain;
    c.b *= gain;
  } else if (behavior == AdjustmentBehavior::Black) {
    const float offset = value * 0.001f;
    c.r += offset;
    c.g += offset;
    c.b += offset;
  } else if (behavior == AdjustmentBehavior::Curve) {
    c.r = ApplyCurve(c.r, p);
    c.g = ApplyCurve(c.g, p);
    c.b = ApplyCurve(c.b, p);
  } else if (behavior == AdjustmentBehavior::Hls) {
    c = ApplyHls(c, p);
  } else if (behavior == AdjustmentBehavior::Saturation ||
             behavior == AdjustmentBehavior::Vibrance) {
    const float l     = Luma(c);
    float       scale = behavior == AdjustmentBehavior::Saturation ? value : 1.0f + value * 0.01f;
    if (behavior == AdjustmentBehavior::Vibrance) {
      const float maximum = std::max(c.r, std::max(c.g, c.b));
      const float minimum = std::min(c.r, std::min(c.g, c.b));
      scale               = 1.0f + (scale - 1.0f) * (1.0f - std::min(maximum - minimum, 1.0f));
    }
    c.r = l + (c.r - l) * scale;
    c.g = l + (c.g - l) * scale;
    c.b = l + (c.b - l) * scale;
  } else if (behavior == AdjustmentBehavior::ColorWheel) {
    const float gamma_x = std::max(p.values[4] + p.values[7], 1.0e-4f);
    const float gamma_y = std::max(p.values[5] + p.values[7], 1.0e-4f);
    const float gamma_z = std::max(p.values[6] + p.values[7], 1.0e-4f);
    c.r = std::copysign(std::pow(std::fabs(c.r + p.values[0] + p.values[3]), 1.0f / gamma_x), c.r) *
          p.values[8];
    c.g = std::copysign(std::pow(std::fabs(c.g + p.values[1] + p.values[3]), 1.0f / gamma_y), c.g) *
          p.values[9];
    c.b = std::copysign(std::pow(std::fabs(c.b + p.values[2] + p.values[3]), 1.0f / gamma_z), c.b) *
          p.values[10];
  }
  return c;
}

auto CpuAcesccEncode(float value) -> float {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) {
    return kFloor + value;
  }
  if (value < kTransition) {
    return (std::log2(kOffset + value * 0.5f) + kA) / kB;
  }
  return (std::log2(value) + kA) / kB;
}

auto CpuAcesccDecode(float value) -> float {
  constexpr float kA         = 9.72f;
  constexpr float kB         = 17.52f;
  constexpr float kOffset    = 0.0000152587890625f;
  constexpr float kFloor     = (-16.0f + kA) / kB;
  constexpr float kThreshold = (-15.0f + kA) / kB;
  if (value < kFloor) {
    return value - kFloor;
  }
  if (value <= kThreshold) {
    return (std::exp2(value * kB - kA) - kOffset) * 2.0f;
  }
  return std::exp2(value * kB - kA);
}

auto CpuLogIntensity(const Rgba& pixel) -> float {
  const float r = CpuAcesccDecode(pixel.r);
  const float g = CpuAcesccDecode(pixel.g);
  const float b = CpuAcesccDecode(pixel.b);
  return CpuAcesccEncode(std::max(0.272229f * r + 0.674082f * g + 0.053689f * b, 1.0e-6f));
}

auto CpuReadRgbaBilinear(const std::vector<Rgba>& pixels, int width, int height, float x, float y)
    -> Rgba {
  x               = std::clamp(x, 0.0f, static_cast<float>(width - 1));
  y               = std::clamp(y, 0.0f, static_cast<float>(height - 1));
  const int   x0  = static_cast<int>(std::floor(x));
  const int   y0  = static_cast<int>(std::floor(y));
  const int   x1  = std::min(x0 + 1, width - 1);
  const int   y1  = std::min(y0 + 1, height - 1);
  const float tx  = x - static_cast<float>(x0);
  const float ty  = y - static_cast<float>(y0);
  const auto  mix = [](const Rgba& a, const Rgba& b, float t) -> Rgba {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
  };
  const Rgba ab = mix(pixels[static_cast<std::size_t>(y0 * width + x0)],
                      pixels[static_cast<std::size_t>(y0 * width + x1)], tx);
  const Rgba cd = mix(pixels[static_cast<std::size_t>(y1 * width + x0)],
                      pixels[static_cast<std::size_t>(y1 * width + x1)], tx);
  return mix(ab, cd, ty);
}

struct CpuPlane {
  int                width  = 1;
  int                height = 1;
  std::vector<float> values;

  CpuPlane() = default;
  CpuPlane(int width_value, int height_value)
      : width(width_value),
        height(height_value),
        values(static_cast<std::size_t>(width_value) * static_cast<std::size_t>(height_value)) {}

  auto At(int x, int y) const -> float {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return values[static_cast<std::size_t>(y * width + x)];
  }

  auto At(int x, int y) -> float& { return values[static_cast<std::size_t>(y * width + x)]; }
};

auto CpuPyramidLevelCount(int width, int height) -> int {
  return local_tone_mapping::ComputeLevelCount(width, height, local_tone_mapping::kPyramidRadius);
}

auto CpuDownsample(const CpuPlane& source, int width, int height) -> CpuPlane {
  CpuPlane destination(width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float sum = 0.0f;
      for (int ky = -2; ky <= 2; ++ky) {
        for (int kx = -2; kx <= 2; ++kx) {
          const float weight   = (kx == -2 || kx == 2)   ? 1.0f / 16.0f
                                 : (kx == -1 || kx == 1) ? 4.0f / 16.0f
                                                         : 6.0f / 16.0f;
          const float y_weight = (ky == -2 || ky == 2)   ? 1.0f / 16.0f
                                 : (ky == -1 || ky == 1) ? 4.0f / 16.0f
                                                         : 6.0f / 16.0f;
          sum += weight * y_weight * source.At(x * 2 + kx, y * 2 + ky);
        }
      }
      destination.At(x, y) = sum;
    }
  }
  return destination;
}

auto CpuBuildPyramid(const CpuPlane& base) -> std::vector<CpuPlane> {
  const int             count = CpuPyramidLevelCount(base.width, base.height);
  std::vector<CpuPlane> levels;
  levels.reserve(static_cast<std::size_t>(count));
  levels.push_back(base);
  for (int level = 1; level < count; ++level) {
    levels.push_back(CpuDownsample(levels.back(), std::max(1, (levels.back().width + 1) / 2),
                                   std::max(1, (levels.back().height + 1) / 2)));
  }
  return levels;
}

auto CpuExpand(const CpuPlane& coarse, int x, int y) -> float {
  const auto weight = [](int tap) -> float {
    return (tap == -2 || tap == 2)   ? 1.0f / 16.0f
           : (tap == -1 || tap == 1) ? 4.0f / 16.0f
                                     : 6.0f / 16.0f;
  };
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) {
      continue;
    }
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) {
        continue;
      }
      sum += 4.0f * weight(kx) * weight(ky) * coarse.At(sample_x / 2, sample_y / 2);
    }
  }
  return sum;
}

auto CpuBilinear(const CpuPlane& plane, float x, float y) -> float {
  x              = std::clamp(x, 0.0f, static_cast<float>(plane.width - 1));
  y              = std::clamp(y, 0.0f, static_cast<float>(plane.height - 1));
  const int   x0 = static_cast<int>(std::floor(x));
  const int   y0 = static_cast<int>(std::floor(y));
  const int   x1 = std::min(x0 + 1, plane.width - 1);
  const int   y1 = std::min(y0 + 1, plane.height - 1);
  const float tx = x - static_cast<float>(x0);
  const float ty = y - static_cast<float>(y0);
  const float a  = plane.At(x0, y0) + (plane.At(x1, y0) - plane.At(x0, y0)) * tx;
  const float b  = plane.At(x0, y1) + (plane.At(x1, y1) - plane.At(x0, y1)) * tx;
  return a + (b - a) * ty;
}

auto CpuApplyLlf(const std::vector<Rgba>& input, int width, int height, float shadows_slider,
                 float highlights_slider) -> std::vector<Rgba> {
  const auto mask_dims = local_tone_mapping::ComputeMaskDimensions(
      width, height, local_tone_mapping::kReferenceMaskMaxLongEdge);
  CpuPlane source_base(mask_dims.width, mask_dims.height);
  for (int y = 0; y < source_base.height; ++y) {
    for (int x = 0; x < source_base.width; ++x) {
      const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(width) /
                           static_cast<float>(source_base.width) -
                       0.5f;
      const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(height) /
                           static_cast<float>(source_base.height) -
                       0.5f;
      source_base.At(x, y) = CpuLogIntensity(CpuReadRgbaBilinear(input, width, height, sx, sy));
    }
  }
  auto        source = CpuBuildPyramid(source_base);

  const float shadow_amount =
      std::clamp(shadows_slider * local_tone_mapping::kHighlightStrengthScale / 80.0f,
                 -local_tone_mapping::kBackendAmountLimit, local_tone_mapping::kBackendAmountLimit);
  const float highlight_amount =
      std::clamp(-highlights_slider * local_tone_mapping::kHighlightStrengthScale / 100.0f,
                 -local_tone_mapping::kBackendAmountLimit, local_tone_mapping::kBackendAmountLimit);
  const float sigma       = local_tone_mapping::SigmaR(shadow_amount, highlight_amount);
  const auto  samples     = local_tone_mapping::BuildSamples(shadow_amount, highlight_amount);

  const auto  build_remap = [&](const local_tone_mapping::LlfSample& sample) {
    CpuPlane base(source_base.width, source_base.height);
    for (std::size_t index = 0; index < base.values.size(); ++index) {
      const float delta     = source_base.values[index] - sample.gamma;
      const float magnitude = std::fabs(delta);
      float       remapped  = 0.0f;
      if (magnitude > 1.0e-6f) {
        const float sign = std::copysign(1.0f, delta);
        if (magnitude <= sigma) {
          remapped = sign * sigma *
                     std::pow(std::min(magnitude / std::max(sigma, 1.0e-6f), 1.0f), sample.alpha);
        } else {
          remapped = sign * (sigma + sample.beta * (magnitude - sigma));
        }
      }
      base.values[index] = sample.target + remapped;
    }
    return CpuBuildPyramid(base);
  };

  auto                  remap_a = build_remap(samples[0]);
  auto                  remap_b = build_remap(samples[1]);
  std::vector<CpuPlane> result;
  result.reserve(source.size());
  for (const auto& level : source) {
    result.emplace_back(level.width, level.height);
  }

  for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
    for (std::size_t level = 0; level < source.size(); ++level) {
      const bool  top         = level + 1 == source.size();
      const auto& low         = remap_a[level];
      const auto& high        = remap_b[level];
      const auto& low_coarse  = top ? remap_a[level] : remap_a[level + 1];
      const auto& high_coarse = top ? remap_b[level] : remap_b[level + 1];
      auto&       destination = result[level];
      for (int y = 0; y < destination.height; ++y) {
        for (int x = 0; x < destination.width; ++x) {
          const float value = source[level].At(x, y);
          if (!((pair == 0 && value <= samples[pair + 1].gamma) ||
                (pair + 2 == samples.size() && value >= samples[pair].gamma) ||
                (value >= samples[pair].gamma && value < samples[pair + 1].gamma))) {
            continue;
          }
          const float t =
              std::clamp((value - samples[pair].gamma) /
                             std::max(samples[pair + 1].gamma - samples[pair].gamma, 1.0e-6f),
                         0.0f, 1.0f);
          if (top) {
            destination.At(x, y) = low.At(x, y) + (high.At(x, y) - low.At(x, y)) * t;
          } else {
            const float low_laplacian  = low.At(x, y) - CpuExpand(low_coarse, x, y);
            const float high_laplacian = high.At(x, y) - CpuExpand(high_coarse, x, y);
            destination.At(x, y)       = low_laplacian + (high_laplacian - low_laplacian) * t;
          }
        }
      }
    }
    if (pair + 2 < samples.size()) {
      std::swap(remap_a, remap_b);
      remap_b = build_remap(samples[pair + 2]);
    }
  }

  for (int level = static_cast<int>(source.size()) - 2; level >= 0; --level) {
    CpuPlane collapsed(source[static_cast<std::size_t>(level)].width,
                       source[static_cast<std::size_t>(level)].height);
    for (int y = 0; y < collapsed.height; ++y) {
      for (int x = 0; x < collapsed.width; ++x) {
        collapsed.At(x, y) = result[static_cast<std::size_t>(level)].At(x, y) +
                             CpuExpand(result[static_cast<std::size_t>(level + 1)], x, y);
      }
    }
    result[static_cast<std::size_t>(level)] = std::move(collapsed);
  }

  const auto&       adjusted = result[0];
  std::vector<Rgba> output(input.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float ax =
          (static_cast<float>(x) + 0.5f) * adjusted.width / static_cast<float>(width) - 0.5f;
      const float ay =
          (static_cast<float>(y) + 0.5f) * adjusted.height / static_cast<float>(height) - 0.5f;
      const float     reference_l      = CpuBilinear(source_base, ax, ay);
      const float     adjusted_l       = CpuBilinear(adjusted, ax, ay);
      const auto&     pixel            = input[static_cast<std::size_t>(y * width + x)];
      const float     source_l         = CpuLogIntensity(pixel);
      const float     source_intensity = std::max(CpuAcesccDecode(source_l), 1.0e-5f);
      const float     target_intensity = CpuAcesccDecode(source_l + adjusted_l - reference_l);
      const float     ratio       = std::clamp(target_intensity / source_intensity, 0.0f, 32.0f);
      float           r           = CpuAcesccDecode(pixel.r) * ratio;
      float           g           = CpuAcesccDecode(pixel.g) * ratio;
      float           b           = CpuAcesccDecode(pixel.b) * ratio;
      constexpr float kLower      = -1.0e-5f;
      float           gamut_scale = 1.0f;
      if (r < kLower && target_intensity > r) {
        gamut_scale = std::min(gamut_scale, (target_intensity - kLower) / (target_intensity - r));
      }
      if (g < kLower && target_intensity > g) {
        gamut_scale = std::min(gamut_scale, (target_intensity - kLower) / (target_intensity - g));
      }
      if (b < kLower && target_intensity > b) {
        gamut_scale = std::min(gamut_scale, (target_intensity - kLower) / (target_intensity - b));
      }
      gamut_scale = std::clamp(gamut_scale, 0.0f, 1.0f);
      r           = target_intensity + (r - target_intensity) * gamut_scale;
      g           = target_intensity + (g - target_intensity) * gamut_scale;
      b           = target_intensity + (b - target_intensity) * gamut_scale;
      output[static_cast<std::size_t>(y * width + x)] = {CpuAcesccEncode(r), CpuAcesccEncode(g),
                                                         CpuAcesccEncode(b), pixel.a};
    }
  }
  return output;
}

void WriteIdentityCube(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE 2\n"
      << "0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
      << "0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
}

void WriteConstantRgbCube(const std::filesystem::path& path, float r, float g, float b) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE 2\n";
  for (int i = 0; i < 8; ++i) {
    out << r << ' ' << g << ' ' << b << '\n';
  }
}

void ResetProductLookToIdentity(PipelineDocument& document) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  auto* saturation = dynamic_cast<SaturationModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(saturation, nullptr);
  exposure->SetValue(0.0f);
  saturation->SetValue(1.0f);
}

auto DownloadR32f(OpenClRenderDevice& device, const GraphValueId& id) -> std::vector<float> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto& texture = lease->Texture();
  EXPECT_EQ(texture.Format(), TextureFormat::R32f);
  std::vector<float> values(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(values.data()),
                           values.size() * sizeof(float)),
      device.CommandContext());
  return values;
}

auto RenderPreparedGrade(OpenClRenderDevice& device, const ExecutionPlan& plan,
                         const PreparedRawInput& prepared, PipelineDocument& document,
                         std::vector<Rgba>* output_pixels  = nullptr,
                         std::vector<Rgba>* develop_pixels = nullptr) -> OpenClPrimaryGradeResult {
  device.BeginRender();
  try {
    ExecuteOpenClDevelop(device, plan, prepared, document);
    ExecuteOpenClGeometryResample(device, plan);
    ExecuteOpenClCameraColor(device, plan, document);
    auto result = ExecuteOpenClPrimaryGrade(device, plan, prepared, document);
    device.EndRender();
    device.WaitIdle();
    if (develop_pixels != nullptr) {
      *develop_pixels = Download(device, plan.develop_output);
    }
    if (output_pixels != nullptr) {
      *output_pixels = Download(device, result.output);
    }
    device.PublishResults();
    return result;
  } catch (...) {
    device.CancelRender();
    throw;
  }
}

auto LocalToneValueId(const NodeId& grade_id, const char* family) -> GraphValueId {
  return {grade_id, PortId{std::string{"local_tone."} + family + ".0"}};
}

class OpenClGradeFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
    device_   = std::make_unique<OpenClRenderDevice>();
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                              gpu_dag_test::FullSensor(16, 12));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
    ResetProductLookToIdentity(document_);
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), RenderRequest{});
  }

  auto RenderGrade() -> OpenClPrimaryGradeResult {
    device_->BeginRender();
    ExecuteOpenClDevelop(*device_, plan_, prepared_, document_);
    ExecuteOpenClGeometryResample(*device_, plan_);
    ExecuteOpenClCameraColor(*device_, plan_, document_);
    auto result = ExecuteOpenClPrimaryGrade(*device_, plan_, prepared_, document_);
    device_->EndRender();
    device_->WaitIdle();
    last_develop_pixels_ = Download(*device_, plan_.develop_output);
    last_output_pixels_  = Download(*device_, result.output);
    device_->PublishResults();
    return result;
  }

  auto RenderThroughDrtPost() -> OpenClDrtResult {
    device_->BeginRender();
    ExecuteOpenClDevelop(*device_, plan_, prepared_, document_);
    ExecuteOpenClGeometryResample(*device_, plan_);
    ExecuteOpenClCameraColor(*device_, plan_, document_);
    auto grade  = ExecuteOpenClPrimaryGrade(*device_, plan_, prepared_, document_);
    auto result = ExecuteOpenClDrt(*device_, plan_, document_);
    device_->EndRender();
    device_->WaitIdle();
    last_develop_pixels_ = Download(*device_, plan_.develop_output);
    last_grade_pixels_   = Download(*device_, grade.output);
    last_output_pixels_  = Download(*device_, result.scene_post);
    device_->PublishResults();
    return result;
  }

  template <class Model>
  auto ModelByType(const OperatorTypeId& type) -> Model& {
    IOperatorModel* found = document_.PrimaryGrade()->FindAdjustmentByType(type);
    if (found == nullptr && document_.Drt() != nullptr) {
      found = document_.Drt()->FindAdjustmentByType(type);
    }
    auto* model = dynamic_cast<Model*>(found);
    EXPECT_NE(model, nullptr);
    return *model;
  }

  void UseNeighborhoodPlane(std::uint32_t width, std::uint32_t height, float surroundings,
                            float center, const RenderRequest& request = {}) {
    prepared_ =
        RawInputLoader::FromDirectRgb(MakeNeighborhoodPlane(width, height, surroundings, center),
                                      gpu_dag_test::FullSensor(width, height));
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), request);
  }

  PreparedRawInput                    prepared_;
  PipelineDocument                    document_;
  ExecutionPlan                       plan_;
  std::unique_ptr<OpenClRenderDevice> device_;
  std::vector<Rgba>                   last_develop_pixels_;
  std::vector<Rgba>                   last_grade_pixels_;
  std::vector<Rgba>                   last_output_pixels_;
};

TEST_F(OpenClGradeFixture, OpenClPrimaryGradePreservesCompiledAdjustmentOrder) {
  ASSERT_NE(plan_.FirstGrade(), nullptr);
  ASSERT_FALSE(plan_.FirstGrade()->adjustments.empty());
  ASSERT_EQ(plan_.FirstGrade()->adjustments.size(), document_.PrimaryGrade()->AdjustmentCount());
  for (std::size_t i = 0; i < plan_.FirstGrade()->adjustments.size(); ++i) {
    EXPECT_EQ(plan_.FirstGrade()->adjustments[i].instance_id,
              document_.PrimaryGrade()->AdjustmentIdAt(i));
    EXPECT_EQ(plan_.FirstGrade()->adjustments[i].type,
              document_.PrimaryGrade()->AdjustmentAt(i).Type());
  }
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(1.0f);
  ModelByType<ContrastModel>(type_ids::Contrast()).SetValue(100.0f);
  const auto  result = RenderGrade();
  const auto& input  = last_develop_pixels_;
  const auto& output = last_output_pixels_;
  ASSERT_FALSE(output.empty());
  EXPECT_NEAR(output.front().r, (input.front().r + 1.0f / 17.52f - 0.18f) * 2.0f + 0.18f, 1.0e-5f);
}

TEST_F(OpenClGradeFixture, OpenClPointwiseAdjustmentsUseOneDispatchPerLlfSegment) {
  const auto identity = RenderGrade();
  EXPECT_EQ(identity.pointwise_dispatch_count, 1U);
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto split = RenderGrade();
  EXPECT_EQ(split.pointwise_dispatch_count, 2U);
  EXPECT_EQ(split.local_tone_pass_count, 1U);
}

TEST_F(OpenClGradeFixture, OpenClSingleSliderEditUploadsOnlyItsParameterRange) {
  (void)RenderGrade();
  auto& exposure = ModelByType<ExposureModel>(type_ids::Exposure());
  exposure.SetValue(1.0f);
  const ParameterSlotKey exposure_key{document_.PrimaryGrade()->Id(),
                                      AdjustmentInstanceId{"grade.primary.exposure"}};
  const auto             exposure_binding = device_->Workspace().Parameters().Binding(exposure_key);
  device_->Workspace().Device().ResetCounters();
  (void)RenderGrade();
  const auto& ranges = device_->Workspace().Device().LastHostToDeviceRanges();
  EXPECT_TRUE(std::ranges::any_of(ranges, [&](const ByteRange& range) {
    return range.offset == exposure_binding.offset && range.size == exposure_binding.size;
  }));
  EXPECT_FALSE(exposure.IsDirty());
  EXPECT_EQ(device_->Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().KernelCreateCount(), 0U);
  EXPECT_NE(device_->Workspace().Device().GradeCommandTopologyHash(), 0U);
}

TEST_F(OpenClGradeFixture, OpenClGradeParameterBindDoesNotCopyFullDto) {
  OperatorModelFullDtoCopyCount::Reset();
  (void)RenderGrade();
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(1.0f);
  OperatorModelFullDtoCopyCount::Reset();
  (void)RenderGrade();
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST_F(OpenClGradeFixture, OpenClCameraColorPackedWriteDoesNotCopyFullDto) {
  device_->BeginRender();
  ExecuteOpenClDevelop(*device_, plan_, prepared_, document_);
  ExecuteOpenClGeometryResample(*device_, plan_);
  OperatorModelFullDtoCopyCount::Reset();
  ExecuteOpenClCameraColor(*device_, plan_, document_);
  device_->EndRender();
  device_->WaitIdle();
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST_F(OpenClGradeFixture, OpenClExposureEditRunsOnlyPrimaryGradeAndDrt) {
  (void)device_->Execute(plan_, prepared_, document_);
  device_->WaitIdle();
  device_->ResetPassStats();
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(0.75f);
  (void)device_->Execute(plan_, prepared_, document_);
  device_->WaitIdle();
  const auto stats = device_->PassStats();
  EXPECT_EQ(stats.source_h2d_count, 0U);
  EXPECT_EQ(stats.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.geometry_execute, 0U);
  EXPECT_EQ(stats.camera_color_execute, 0U);
  EXPECT_EQ(stats.primary_grade_execute, 1U);
  EXPECT_EQ(stats.drt_execute, 1U);
  EXPECT_EQ(stats.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.geometry_skip, 1U);
  EXPECT_EQ(stats.camera_color_skip, 1U);
}

TEST_F(OpenClGradeFixture, OpenClLutRemapChangesGradePixels) {
  const auto cube_path = std::filesystem::absolute("build/tmp/gpu_dag_opencl_lut/red.cube");
  WriteConstantRgbCube(cube_path, 1.0f, 0.0f, 0.0f);
  auto* lmt =
      dynamic_cast<LmtModel*>(document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()));
  ASSERT_NE(lmt, nullptr);
  lmt->SetCubePath(cube_path.string());
  const auto result = RenderGrade();
  ASSERT_FALSE(last_output_pixels_.empty());
  ASSERT_FALSE(last_develop_pixels_.empty());
  EXPECT_NE(result.lut_resource_id, 0U);
  EXPECT_NEAR(last_output_pixels_.front().r, 1.0f, 1.0e-4f);
  EXPECT_NEAR(last_output_pixels_.front().g, 0.0f, 1.0e-4f);
  EXPECT_NEAR(last_output_pixels_.front().b, 0.0f, 1.0e-4f);
  EXPECT_GT(std::abs(last_output_pixels_.front().r - last_develop_pixels_.front().r) +
                std::abs(last_output_pixels_.front().g - last_develop_pixels_.front().g) +
                std::abs(last_output_pixels_.front().b - last_develop_pixels_.front().b),
            1.0e-3f);
}

TEST_F(OpenClGradeFixture, OpenClLutResourceIsReusedByContentKey) {
  const auto cube_path = std::filesystem::absolute("build/tmp/gpu_dag_opencl_o2/identity.cube");
  WriteIdentityCube(cube_path);
  auto* lmt =
      dynamic_cast<LmtModel*>(document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()));
  ASSERT_NE(lmt, nullptr);
  lmt->SetCubePath(cube_path.string());
  const auto first = RenderGrade();
  EXPECT_NE(first.lut_resource_id, 0U);
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(0.25f);
  device_->Workspace().Device().ResetCounters();
  const auto second = RenderGrade();
  EXPECT_EQ(second.lut_resource_id, first.lut_resource_id);
  EXPECT_EQ(device_->Workspace().Device().LutUploadBytes(), 0U);
  EXPECT_EQ(device_->Workspace().Device().LastLutResourceId(), first.lut_resource_id);
  EXPECT_EQ(device_->Workspace().Device().BufferCreateCount(), 0U);
}

TEST_F(OpenClGradeFixture, OpenClDetailPassesAcquireAllResourcesFromWorkspace) {
  ModelByType<ClarityModel>(type_ids::Clarity()).SetValue(20.0f);
  ModelByType<SharpenModel>(type_ids::Sharpen()).SetAmount(10.0f);
  ModelByType<HalationModel>(type_ids::Halation()).SetValue(0.4f);
  ModelByType<FilmGrainModel>(type_ids::FilmGrain()).SetValue(0.3f);
  const auto first = RenderThroughDrtPost();
  EXPECT_EQ(first.post_neighborhood_count, 4U);
  EXPECT_GT(device_->Workspace().Textures().EntryCount(), 0U);
  device_->Workspace().Device().ResetCounters();
  const auto second = RenderThroughDrtPost();
  EXPECT_EQ(second.post_neighborhood_count, 4U);
  EXPECT_EQ(device_->Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().KernelCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().ProgramBuildCount(), 0U);
}

TEST_F(OpenClGradeFixture, OpenClSharpenUsesSurroundingPixelsForUnsharpMask) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  UseNeighborhoodPlane(width, height, 0.18f, 0.55f);
  auto& sharpen = ModelByType<SharpenModel>(type_ids::Sharpen());
  sharpen.SetAmount(100.0f);
  sharpen.SetRadius(3.0f);
  sharpen.SetThreshold(0.0f);

  const auto  result         = RenderThroughDrtPost();
  const auto& input          = last_grade_pixels_;
  const auto& output         = last_output_pixels_;
  const auto  center         = static_cast<std::size_t>(height / 2) * width + width / 2;
  const auto  neighbor_index = center - 1;
  const auto  far_index      = static_cast<std::size_t>(height / 2) * width + 2;
  ASSERT_EQ(input.size(), output.size());
  EXPECT_EQ(result.post_neighborhood_count, 1U);
  EXPECT_GT(output[center].r, input[center].r);
  EXPECT_LT(output[neighbor_index].r, input[neighbor_index].r);
  EXPECT_NEAR(output[far_index].r, input[far_index].r, 1.0e-6f);
}

TEST_F(OpenClGradeFixture, OpenClSharpenDarkRingFollowsPreviewResolution) {
  // Radius 4 is 12 taps at 1:1 and 6 taps at render_scale 0.5. Offset 10 sits between those
  // radii, so an unscaled kernel would still darken the half-res probe.
  constexpr std::uint32_t width   = 128;
  constexpr std::uint32_t height = 128;
  auto&                   sharpen = ModelByType<SharpenModel>(type_ids::Sharpen());
  sharpen.SetRadius(4.0f);
  sharpen.SetThreshold(0.0f);

  UseNeighborhoodPlane(width, height, 0.18f, 0.55f);
  sharpen.SetAmount(0.0f);
  (void)RenderThroughDrtPost();
  const auto full_identity = last_output_pixels_;
  sharpen.SetAmount(100.0f);
  (void)RenderThroughDrtPost();
  const auto full_sharpened = last_output_pixels_;
  const auto full_near =
      static_cast<std::size_t>(height / 2) * width + width / 2 + 1U;
  ASSERT_EQ(full_identity.size(), full_sharpened.size());
  EXPECT_GT(full_identity[full_near].r - full_sharpened[full_near].r, 1.0e-4f);

  RenderRequest half_request;
  half_request.resolution.render_scale = 0.5f;
  UseNeighborhoodPlane(width, height, 0.18f, 0.55f, half_request);
  sharpen.SetAmount(0.0f);
  (void)RenderThroughDrtPost();
  const auto half_identity = last_output_pixels_;
  sharpen.SetAmount(100.0f);
  const auto half_result    = RenderThroughDrtPost();
  const auto half_sharpened = last_output_pixels_;
  ASSERT_EQ(half_identity.size(), static_cast<std::size_t>(64) * 64);
  EXPECT_EQ(half_result.post_neighborhood_count, 1U);
  const auto half_near = static_cast<std::size_t>(32) * 64U + 32U + 1U;
  const auto half_far  = static_cast<std::size_t>(32) * 64U + 32U + 10U;
  EXPECT_GT(half_identity[half_near].r - half_sharpened[half_near].r, 1.0e-5f);
  EXPECT_NEAR(half_sharpened[half_far].r, half_identity[half_far].r, 2.0e-3f);
}

TEST_F(OpenClGradeFixture, OpenClClarityUsesLargeRadiusLocalContrast) {
  constexpr std::uint32_t width  = 96;
  constexpr std::uint32_t height = 96;
  UseNeighborhoodPlane(width, height, 0.22f, 0.48f);
  ModelByType<ClarityModel>(type_ids::Clarity()).SetValue(80.0f);

  const auto  result         = RenderThroughDrtPost();
  const auto& input          = last_grade_pixels_;
  const auto& output         = last_output_pixels_;
  const auto  center         = static_cast<std::size_t>(height / 2) * width + width / 2;
  const auto  neighbor_index = center - 1;
  const auto  far_index      = static_cast<std::size_t>(height / 2) * width + 1;
  ASSERT_EQ(input.size(), output.size());
  EXPECT_EQ(result.post_neighborhood_count, 1U);
  EXPECT_GT(output[center].r, input[center].r);
  EXPECT_LT(output[neighbor_index].r, input[neighbor_index].r);
  EXPECT_NEAR(output[far_index].r, input[far_index].r, 1.0e-6f);
}

TEST_F(OpenClGradeFixture, OpenClHalationSpreadsRedLightIntoDarkNeighbors) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  UseNeighborhoodPlane(width, height, 0.02f, 1.0f);
  ModelByType<HalationModel>(type_ids::Halation()).SetValue(1.0f);

  const auto  result         = RenderThroughDrtPost();
  const auto& input          = last_grade_pixels_;
  const auto& output         = last_output_pixels_;
  const auto  center         = static_cast<std::size_t>(height / 2) * width + width / 2;
  const auto  neighbor_index = center - 1;
  const auto  far_index      = static_cast<std::size_t>(height / 2) * width + 2;
  ASSERT_EQ(input.size(), output.size());
  EXPECT_EQ(result.post_neighborhood_count, 1U);
  const float red_spill   = output[neighbor_index].r - input[neighbor_index].r;
  const float green_spill = output[neighbor_index].g - input[neighbor_index].g;
  EXPECT_GT(red_spill, 1.0e-4f);
  EXPECT_GT(red_spill, green_spill * 5.0f);
  EXPECT_NEAR(output[far_index].r, input[far_index].r, 1.0e-6f);
}

TEST_F(OpenClGradeFixture, OpenClFilmGrainStrengthScalesDeterministicDensityVariation) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  UseNeighborhoodPlane(width, height, 0.35f, 0.35f);
  auto& grain = ModelByType<FilmGrainModel>(type_ids::FilmGrain());

  grain.SetValue(0.25f);
  (void)RenderThroughDrtPost();
  const auto low   = last_output_pixels_;
  const auto input = last_grade_pixels_;
  grain.SetValue(0.75f);
  (void)RenderThroughDrtPost();
  const auto high = last_output_pixels_;
  (void)RenderThroughDrtPost();
  const auto high_again = last_output_pixels_;
  ASSERT_EQ(input.size(), high.size());

  double low_energy  = 0.0;
  double high_energy = 0.0;
  for (std::size_t index = 0; index < input.size(); ++index) {
    low_energy += std::pow(static_cast<double>(low[index].r - input[index].r), 2.0);
    high_energy += std::pow(static_cast<double>(high[index].r - input[index].r), 2.0);
    EXPECT_FLOAT_EQ(high[index].r, high_again[index].r);
    EXPECT_FLOAT_EQ(high[index].g, high_again[index].g);
    EXPECT_FLOAT_EQ(high[index].b, high_again[index].b);
  }
  EXPECT_GT(high_energy, low_energy * 6.0);
}

TEST_F(OpenClGradeFixture, OpenClPrimaryGradeMatchesCudaReferenceWithinTolerance) {
  ModelByType<Cat02WhiteBalanceModel>(type_ids::Cat02WhiteBalance()).SetTemperatureOffset(120.0f);
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(0.5f);
  ModelByType<ContrastModel>(type_ids::Contrast()).SetValue(40.0f);
  ModelByType<WhiteModel>(type_ids::White()).SetValue(12.0f);
  ModelByType<SaturationModel>(type_ids::Saturation()).SetValue(1.2f);
  const auto  result = RenderGrade();
  const auto& input  = last_develop_pixels_;
  const auto& output = last_output_pixels_;
  ASSERT_EQ(input.size(), output.size());
  ASSERT_FALSE(input.empty());

  std::vector<GradeAdjustmentParams> params;
  auto*                              grade = document_.PrimaryGrade();
  ASSERT_NE(plan_.FirstGrade(), nullptr);
  for (const auto& compiled : plan_.FirstGrade()->adjustments) {
    auto* model = grade->FindAdjustment(compiled.instance_id);
    ASSERT_NE(model, nullptr);
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value() || IsLocalToneBehavior(*behavior)) {
      continue;
    }
    if (compiled.algorithm == CompiledAdjustmentAlgorithm::Neighborhood &&
        MakeGradeRuntimeParams(*model, *behavior).values[0] == 0.0f) {
      continue;
    }
    params.push_back(MakeGradeRuntimeParams(*model, *behavior));
  }

  float max_err = 0.0f;
  for (std::size_t i = 0; i < input.size(); ++i) {
    Rgba cpu = input[i];
    for (const auto& p : params) {
      cpu = CpuApplyAdjustment(cpu, p);
    }
    max_err = std::max(max_err, std::fabs(cpu.r - output[i].r));
    max_err = std::max(max_err, std::fabs(cpu.g - output[i].g));
    max_err = std::max(max_err, std::fabs(cpu.b - output[i].b));
  }
  EXPECT_LT(max_err, 2.0e-4f);
}

TEST_F(OpenClGradeFixture, OpenClUnknownAdjustmentIsRejectedAtInsert) {
  EXPECT_THROW(
      document_.InsertAdjustment(document_.PrimaryGrade()->Id(),
                                 document_.PrimaryGrade()->AdjustmentCount(),
                                 AdjustmentInstanceId{"grade.primary.tint"},
                                 std::make_unique<TintModel>()),
      std::runtime_error);
}

TEST_F(OpenClGradeFixture, OpenClStableGradeCreatesNoDummyLutOrStageParameterBuffer) {
  device_->Workspace().Device().WarmUpPlan(plan_);
  const auto first = RenderGrade();
  EXPECT_NE(first.lut_resource_id, 0U);
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(0.25f);
  device_->Workspace().Device().ResetCounters();
  const auto second = RenderGrade();
  EXPECT_EQ(second.command_upload_bytes, 0U);
  EXPECT_EQ(device_->Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().KernelCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().ProgramBuildCount(), 0U);
}

TEST_F(OpenClGradeFixture, OpenClLlfUsesWorkspaceTransientArenaForEveryPyramid) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(80.0f);
  const auto result = RenderGrade();
  ASSERT_TRUE(result.local_tone_rebuilt_reference);
  EXPECT_FALSE(result.local_tone_sampled_canonical_reference);

  const auto canonical = local_tone_mapping::ComputeMaskDimensions(
      static_cast<int>(plan_.geometry.full_reference_extent.width),
      static_cast<int>(plan_.geometry.full_reference_extent.height),
      local_tone_mapping::kReferenceMaskMaxLongEdge);
  const auto expected_bytes = local_tone_mapping::EstimateLlfTransientBytes(
      canonical.width, canonical.height, TransientBufferArena<OpenClBackend>::kDefaultAlignment);
  EXPECT_GE(plan_.peak_transient_bytes, expected_bytes);
  std::size_t raw_plane_bytes = 0;
  int         width           = canonical.width;
  int         height          = canonical.height;
  const int   level_count =
      local_tone_mapping::ComputeLevelCount(width, height, local_tone_mapping::kPyramidRadius);
  for (int level = 0; level < level_count; ++level) {
    raw_plane_bytes +=
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(float);
    width  = std::max(1, (width + 1) / 2);
    height = std::max(1, (height + 1) / 2);
  }
  EXPECT_GE(result.local_tone_transient_bytes, 4U * raw_plane_bytes);
  EXPECT_LE(result.local_tone_transient_bytes, expected_bytes);
  EXPECT_GE(device_->Workspace().TransientBuffers().used_bytes(),
            result.local_tone_transient_bytes);
  EXPECT_GE(device_->Workspace().TransientBuffers().capacity_bytes(), plan_.peak_transient_bytes);

  const auto  source_id     = LocalToneValueId(document_.PrimaryGrade()->Id(), "source");
  const auto  result_id     = LocalToneValueId(document_.PrimaryGrade()->Id(), "result");
  const auto  source_key    = HashLlfSourceKey(plan_, prepared_, document_);
  const auto  reference_key = HashLlfReferenceKey(plan_, prepared_, document_);
  const auto* source        = device_->Workspace().Images().Find(source_id);
  const auto* adjusted      = device_->Workspace().Images().Find(result_id);
  ASSERT_NE(source, nullptr);
  ASSERT_NE(adjusted, nullptr);
  EXPECT_EQ(source->Texture().Format(), TextureFormat::R32f);
  EXPECT_EQ(adjusted->Texture().Format(), TextureFormat::R32f);
  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(source_id), source_key);
  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(result_id), reference_key);
  EXPECT_EQ(device_->Workspace().Values().Find(source_id), nullptr);
  EXPECT_EQ(device_->Workspace().Values().Find(result_id), nullptr);
}

TEST_F(OpenClGradeFixture, OpenClLlfFullFrameBuildsCanonicalReferenceOnce) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(55.0f);
  const auto first = RenderGrade();
  ASSERT_TRUE(first.local_tone_rebuilt_reference);
  EXPECT_FALSE(first.local_tone_sampled_canonical_reference);
  ASSERT_NE(first.local_tone_reference_resource_id, 0U);

  const auto source_id     = LocalToneValueId(document_.PrimaryGrade()->Id(), "source");
  const auto result_id     = LocalToneValueId(document_.PrimaryGrade()->Id(), "result");
  const auto source_key    = HashLlfSourceKey(plan_, prepared_, document_);
  const auto reference_key = HashLlfReferenceKey(plan_, prepared_, document_);
  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(source_id), source_key);
  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(result_id), reference_key);
  EXPECT_EQ(device_->Workspace().Images().PublishedAuxiliary(source_id, source_key), 16U);

  const auto second = RenderGrade();
  EXPECT_FALSE(second.local_tone_rebuilt_reference);
  EXPECT_TRUE(second.local_tone_sampled_canonical_reference);
  EXPECT_EQ(second.local_tone_reference_resource_id, first.local_tone_reference_resource_id);
  EXPECT_LE(device_->Workspace().Images().PublishedLastWriter(source_id, source_key),
            device_->Workspace().Device().CompletedSubmission());
}

TEST_F(OpenClGradeFixture, OpenClLlfSliderEditReusesCanonicalReference) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto first               = RenderGrade();
  const auto source_id           = LocalToneValueId(document_.PrimaryGrade()->Id(), "source");
  const auto result_id           = LocalToneValueId(document_.PrimaryGrade()->Id(), "result");
  const auto source_key          = HashLlfSourceKey(plan_, prepared_, document_);
  const auto first_reference_key = HashLlfReferenceKey(plan_, prepared_, document_);
  ASSERT_TRUE(first.local_tone_rebuilt_reference);

  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(70.0f);
  const auto second               = RenderGrade();
  const auto second_reference_key = HashLlfReferenceKey(plan_, prepared_, document_);
  EXPECT_EQ(HashLlfSourceKey(plan_, prepared_, document_), source_key);
  EXPECT_NE(second_reference_key, first_reference_key);
  EXPECT_TRUE(second.local_tone_rebuilt_reference);
  EXPECT_FALSE(second.local_tone_sampled_canonical_reference);
  EXPECT_EQ(second.local_tone_reference_resource_id, first.local_tone_reference_resource_id);
  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(source_id), source_key);
  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(result_id), second_reference_key);
}

TEST_F(OpenClGradeFixture, OpenClLlfSecondStableRenderCreatesNoBufferImageProgramOrKernel) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(60.0f);
  (void)RenderGrade();
  device_->Workspace().Device().ResetCounters();
  const auto second = RenderGrade();
  EXPECT_FALSE(second.local_tone_rebuilt_reference);
  EXPECT_TRUE(second.local_tone_sampled_canonical_reference);
  EXPECT_EQ(device_->Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().ProgramBuildCount(), 0U);
  EXPECT_EQ(device_->Workspace().Device().KernelCreateCount(), 0U);
}

TEST_F(OpenClGradeFixture, OpenClLlfFailedSubmissionDoesNotPublishReference) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(65.0f);
  const auto first = RenderGrade();
  ASSERT_TRUE(first.local_tone_rebuilt_reference);
  const auto source_id          = LocalToneValueId(document_.PrimaryGrade()->Id(), "source");
  const auto result_id          = LocalToneValueId(document_.PrimaryGrade()->Id(), "result");
  const auto source_key         = HashLlfSourceKey(plan_, prepared_, document_);
  const auto reference_key      = HashLlfReferenceKey(plan_, prepared_, document_);
  const auto source_resource_id = first.local_tone_reference_resource_id;

  auto       failed_plan        = plan_;
  failed_plan.geometry.full_reference_extent = {};
  device_->BeginRender();
  ExecuteOpenClDevelop(*device_, failed_plan, prepared_, document_);
  ExecuteOpenClGeometryResample(*device_, failed_plan);
  ExecuteOpenClCameraColor(*device_, failed_plan, document_);
  EXPECT_THROW((void)ExecuteOpenClPrimaryGrade(*device_, failed_plan, prepared_, document_),
               std::runtime_error);
  device_->CancelRender();

  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(source_id), source_key);
  EXPECT_EQ(device_->Workspace().Images().PublishedContentKey(result_id), reference_key);
  EXPECT_EQ(device_->Workspace().Images().PublishedAuxiliary(source_id, source_key), 16U);

  const auto recovered = RenderGrade();
  EXPECT_FALSE(recovered.local_tone_rebuilt_reference);
  EXPECT_TRUE(recovered.local_tone_sampled_canonical_reference);
  EXPECT_EQ(recovered.local_tone_reference_resource_id, source_resource_id);
}

TEST_F(OpenClGradeFixture, OpenClLlfMatchesCudaReferenceWithinTolerance) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(80.0f);
  const auto  result = RenderGrade();
  const auto& input  = last_develop_pixels_;
  const auto& output = last_output_pixels_;
  ASSERT_EQ(input.size(), output.size());
  ASSERT_FALSE(input.empty());

  const auto expected =
      CpuApplyLlf(input, static_cast<int>(plan_.geometry.render_extent.width),
                  static_cast<int>(plan_.geometry.render_extent.height), 80.0f, 0.0f);
  ASSERT_EQ(expected.size(), output.size());
  float max_error = 0.0f;
  for (std::size_t index = 0; index < output.size(); ++index) {
    max_error = std::max(max_error, std::fabs(expected[index].r - output[index].r));
    max_error = std::max(max_error, std::fabs(expected[index].g - output[index].g));
    max_error = std::max(max_error, std::fabs(expected[index].b - output[index].b));
    EXPECT_TRUE(std::isfinite(output[index].r));
    EXPECT_TRUE(std::isfinite(output[index].g));
    EXPECT_TRUE(std::isfinite(output[index].b));
  }
  EXPECT_LT(max_error, 2.0e-3f);
}

TEST_F(OpenClGradeFixture, OpenClLlfPassDoesNotFinishTheQueue) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(60.0f);
  device_->Workspace().Device().ResetCounters();
  device_->BeginRender();
  ExecuteOpenClDevelop(*device_, plan_, prepared_, document_);
  ExecuteOpenClGeometryResample(*device_, plan_);
  ExecuteOpenClCameraColor(*device_, plan_, document_);
  const auto waits_before = device_->Workspace().Device().WaitCount();
  const auto result       = ExecuteOpenClPrimaryGrade(*device_, plan_, prepared_, document_);
  EXPECT_TRUE(result.local_tone_rebuilt_reference);
  EXPECT_EQ(device_->Workspace().Device().WaitCount(), waits_before);
  device_->EndRender();
  device_->WaitIdle();
  device_->PublishResults();
}

TEST(GpuDagOpenClGrade, OpenClLlfRoiSamplesCanonicalReferenceWithSharedGeometryPlan) {
  if (!TryInitializeOpenClRuntime()) {
    GTEST_SKIP() << "No OpenCL device available.";
  }
  constexpr std::uint32_t kWidth   = 64;
  constexpr std::uint32_t kHeight  = 64;
  const auto              prepared = RawInputLoader::FromDirectRgb(
      gpu_dag_test::MakeF32RgbaPlane(kWidth, kHeight), gpu_dag_test::FullSensor(kWidth, kHeight));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(75.0f);

  const auto full_plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  OpenClRenderDevice device;
  const auto         full_result = RenderPreparedGrade(device, full_plan, prepared, document);
  ASSERT_TRUE(full_result.local_tone_rebuilt_reference);
  ASSERT_FALSE(full_result.local_tone_sampled_canonical_reference);
  ASSERT_NE(full_result.local_tone_reference_resource_id, 0U);

  const auto    full_source_key    = HashLlfSourceKey(full_plan, prepared, document);
  const auto    full_reference_key = HashLlfReferenceKey(full_plan, prepared, document);
  const auto    source_id          = LocalToneValueId(document.PrimaryGrade()->Id(), "source");
  const auto    result_id          = LocalToneValueId(document.PrimaryGrade()->Id(), "result");
  const auto    full_sampling = MakeLlfSamplingPlan(full_plan.geometry, Extent2D{kWidth, kHeight});

  RenderRequest roi_request;
  roi_request.view.visible_rect_in_edit_space = NormalizedRect{0.25f, 0.0f, 0.5f, 1.0f};
  roi_request.view.viewport_extent            = Extent2D{32, kHeight};
  const auto roi_plan = GraphCompiler::Compile(document, prepared.CompileSource(), roi_request);
  EXPECT_EQ(HashLlfSourceKey(roi_plan, prepared, document), full_source_key);
  EXPECT_EQ(HashLlfReferenceKey(roi_plan, prepared, document), full_reference_key);
  const auto roi_sampling = MakeLlfSamplingPlan(roi_plan.geometry, Extent2D{kWidth, kHeight});
  EXPECT_NE(roi_sampling.render_to_texture_uv.m[2], full_sampling.render_to_texture_uv.m[2]);

  std::vector<Rgba> roi_pixels;
  const auto roi_result = RenderPreparedGrade(device, roi_plan, prepared, document, &roi_pixels);
  EXPECT_FALSE(roi_result.local_tone_rebuilt_reference);
  EXPECT_TRUE(roi_result.local_tone_sampled_canonical_reference);
  EXPECT_EQ(roi_result.local_tone_reference_resource_id,
            full_result.local_tone_reference_resource_id);
  EXPECT_EQ(device.Workspace().Images().PublishedContentKey(source_id), full_source_key);
  EXPECT_EQ(device.Workspace().Images().PublishedContentKey(result_id), full_reference_key);
  EXPECT_EQ(roi_pixels.size(), static_cast<std::size_t>(32) * kHeight);
  for (const auto& pixel : roi_pixels) {
    EXPECT_TRUE(std::isfinite(pixel.r));
    EXPECT_TRUE(std::isfinite(pixel.g));
    EXPECT_TRUE(std::isfinite(pixel.b));
  }

  OpenClRenderDevice isolated_device;
  const auto isolated_result = RenderPreparedGrade(isolated_device, roi_plan, prepared, document);
  EXPECT_TRUE(isolated_result.local_tone_rebuilt_reference);
  EXPECT_FALSE(isolated_result.local_tone_sampled_canonical_reference);
  EXPECT_EQ(isolated_result.local_tone_reference_resource_id, 0U);
  EXPECT_EQ(isolated_device.Workspace().Images().PublishedCount(), 0U);
}

}  // namespace
}  // namespace alcedo
