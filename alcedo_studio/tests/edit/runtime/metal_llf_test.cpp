//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

auto MakeSplitPlane(std::uint32_t width, std::uint32_t height, float left, float right)
    -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* p) { delete[] p; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float value = x < width / 2 ? left : right;
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

auto MakeNeighborhoodPlane(std::uint32_t width, std::uint32_t height, float surroundings,
                           float center) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* p) { delete[] p; });
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

auto DownloadRgba(MetalRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       tex = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(tex.Width()) * tex.Height());
  device.Workspace().Device().DownloadTexture2D(
      tex,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto DownloadPlane(MetalRenderDevice& device, const NodeId& grade_id, const char* family)
    -> std::vector<float> {
  auto* buffer = device.Workspace().Values().Find(grade_id, PortId{family});
  EXPECT_NE(buffer, nullptr);
  if (buffer == nullptr) {
    return {};
  }
  std::vector<float> plane(buffer->Bytes() / sizeof(float));
  device.Workspace().Device().DownloadBufferRange(
      *buffer, 0, std::span<std::byte>(reinterpret_cast<std::byte*>(plane.data()), buffer->Bytes()),
      device.CommandContext());
  return plane;
}

auto RenderPreparedGrade(MetalRenderDevice& device, PipelineDocument& document,
                         const PreparedRawInput& prepared, const ExecutionPlan& plan)
    -> MetalPrimaryGradeResult {
  device.BeginRender();
  ExecuteMetalDevelop(device, plan, prepared, document);
  ExecuteMetalGeometryResample(device, plan);
  ExecuteMetalCameraColor(device, plan, document);
  auto result = ExecuteMetalPrimaryGrade(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();
  return result;
}

auto AcesccEncode(float value) -> float {
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

auto AcesccDecode(float value) -> float {
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

auto LogIntensity(const Rgba& pixel) -> float {
  const Rgba  linear{AcesccDecode(pixel.r), AcesccDecode(pixel.g), AcesccDecode(pixel.b), pixel.a};
  const float intensity = 0.272229f * linear.r + 0.674082f * linear.g + 0.053689f * linear.b;
  return AcesccEncode(std::max(intensity, 1.0e-6f));
}

auto CpuApplyLlf(const std::vector<Rgba>& input, std::uint32_t width, std::uint32_t height,
                 float shadows_slider, float highlights_slider) -> std::vector<Rgba> {
  const auto mask =
      local_tone_mapping::ComputeMaskDimensions(static_cast<int>(width), static_cast<int>(height),
                                                local_tone_mapping::kReferenceMaskMaxLongEdge);
  const int        count = local_tone_mapping::ComputeLevelCount(mask.width, mask.height,
                                                                 local_tone_mapping::kPyramidRadius);
  std::vector<int> widths(static_cast<std::size_t>(count));
  std::vector<int> heights(static_cast<std::size_t>(count));
  widths[0]  = mask.width;
  heights[0] = mask.height;
  for (int level = 1; level < count; ++level) {
    widths[static_cast<std::size_t>(level)] =
        std::max(1, (widths[static_cast<std::size_t>(level - 1)] + 1) / 2);
    heights[static_cast<std::size_t>(level)] =
        std::max(1, (heights[static_cast<std::size_t>(level - 1)] + 1) / 2);
  }
  auto make_plane = [&](int level) {
    return std::vector<float>(static_cast<std::size_t>(widths[static_cast<std::size_t>(level)]) *
                              static_cast<std::size_t>(heights[static_cast<std::size_t>(level)]));
  };
  auto read_rgba = [&](float x, float y) -> Rgba {
    x               = std::clamp(x, 0.0f, static_cast<float>(width - 1));
    y               = std::clamp(y, 0.0f, static_cast<float>(height - 1));
    const int   x0  = static_cast<int>(std::floor(x));
    const int   y0  = static_cast<int>(std::floor(y));
    const int   x1  = std::min(x0 + 1, static_cast<int>(width - 1));
    const int   y1  = std::min(y0 + 1, static_cast<int>(height - 1));
    const float tx  = x - static_cast<float>(x0);
    const float ty  = y - static_cast<float>(y0);
    const auto& a   = input[static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x0)];
    const auto& b   = input[static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x1)];
    const auto& c   = input[static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x0)];
    const auto& d   = input[static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x1)];
    auto        mix = [&](const Rgba& p, const Rgba& q, float t) {
      return Rgba{p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t,
                  p.a + (q.a - p.a) * t};
    };
    const auto ab = mix(a, b, tx);
    const auto cd = mix(c, d, tx);
    return mix(ab, cd, ty);
  };
  auto plane_read = [](const std::vector<float>& src, int x, int y, int w, int h) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return src[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
               static_cast<std::size_t>(x)];
  };
  auto weight = [](int tap) {
    return (tap == -2 || tap == 2)   ? 1.0f / 16.0f
           : (tap == -1 || tap == 1) ? 4.0f / 16.0f
                                     : 6.0f / 16.0f;
  };
  auto expand = [&](const std::vector<float>& coarse, int cw, int ch, int x, int y) {
    float sum = 0.0f;
    for (int ky = -2; ky <= 2; ++ky) {
      const int sample_y = y - ky;
      if ((sample_y & 1) != 0) {
        continue;
      }
      const int cy = std::clamp(sample_y / 2, 0, ch - 1);
      for (int kx = -2; kx <= 2; ++kx) {
        const int sample_x = x - kx;
        if ((sample_x & 1) != 0) {
          continue;
        }
        const int cx = std::clamp(sample_x / 2, 0, cw - 1);
        sum += 4.0f * weight(kx) * weight(ky) *
               coarse[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cw) +
                      static_cast<std::size_t>(cx)];
      }
    }
    return sum;
  };
  auto pyr_down = [&](const std::vector<float>& src, int sw, int sh, int dw, int dh) {
    std::vector<float> dst(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh));
    for (int y = 0; y < dh; ++y) {
      for (int x = 0; x < dw; ++x) {
        float sum = 0.0f;
        for (int ky = -2; ky <= 2; ++ky) {
          for (int kx = -2; kx <= 2; ++kx) {
            sum += weight(kx) * weight(ky) * plane_read(src, x * 2 + kx, y * 2 + ky, sw, sh);
          }
        }
        dst[static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) +
            static_cast<std::size_t>(x)] = sum;
      }
    }
    return dst;
  };

  std::vector<std::vector<float>> source(static_cast<std::size_t>(count));
  source[0] = make_plane(0);
  for (int y = 0; y < mask.height; ++y) {
    for (int x = 0; x < mask.width; ++x) {
      const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(width) /
                           static_cast<float>(mask.width) -
                       0.5f;
      const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(height) /
                           static_cast<float>(mask.height) -
                       0.5f;
      source[0][static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.width) +
                static_cast<std::size_t>(x)] = LogIntensity(read_rgba(sx, sy));
    }
  }
  for (int level = 1; level < count; ++level) {
    source[static_cast<std::size_t>(level)] = pyr_down(
        source[static_cast<std::size_t>(level - 1)], widths[static_cast<std::size_t>(level - 1)],
        heights[static_cast<std::size_t>(level - 1)], widths[static_cast<std::size_t>(level)],
        heights[static_cast<std::size_t>(level)]);
  }

  const float shadow_amount    = std::clamp(shadows_slider * 1.5f / 80.0f, -1.5f, 1.5f);
  const float highlight_amount = std::clamp(-highlights_slider * 1.5f / 100.0f, -1.5f, 1.5f);
  const float sigma            = local_tone_mapping::SigmaR(shadow_amount, highlight_amount);
  const auto  samples          = local_tone_mapping::BuildSamples(shadow_amount, highlight_amount);
  auto        remap_delta      = [](float delta, float sample_sigma, float alpha, float beta) {
    const float magnitude = std::fabs(delta);
    if (magnitude <= 1.0e-6f) {
      return 0.0f;
    }
    const float sign = std::copysign(1.0f, delta);
    if (magnitude <= sample_sigma) {
      return sign * sample_sigma *
             std::pow(std::min(magnitude / std::max(sample_sigma, 1.0e-6f), 1.0f), alpha);
    }
    return sign * (sample_sigma + beta * (magnitude - sample_sigma));
  };
  auto build_remap = [&](const local_tone_mapping::LlfSample& sample) {
    std::vector<std::vector<float>> levels(static_cast<std::size_t>(count));
    levels[0] = make_plane(0);
    for (std::size_t i = 0; i < levels[0].size(); ++i) {
      levels[0][i] = sample.target +
                     remap_delta(source[0][i] - sample.gamma, sigma, sample.alpha, sample.beta);
    }
    for (int level = 1; level < count; ++level) {
      levels[static_cast<std::size_t>(level)] = pyr_down(
          levels[static_cast<std::size_t>(level - 1)], widths[static_cast<std::size_t>(level - 1)],
          heights[static_cast<std::size_t>(level - 1)], widths[static_cast<std::size_t>(level)],
          heights[static_cast<std::size_t>(level)]);
    }
    return levels;
  };
  auto                            remap_a = build_remap(samples[0]);
  auto                            remap_b = build_remap(samples[1]);
  std::vector<std::vector<float>> result(static_cast<std::size_t>(count));
  for (int level = 0; level < count; ++level) {
    result[static_cast<std::size_t>(level)] = make_plane(level);
  }
  for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
    for (int level = 0; level < count; ++level) {
      const bool top = level + 1 == count;
      const int  w   = widths[static_cast<std::size_t>(level)];
      const int  h   = heights[static_cast<std::size_t>(level)];
      const int  cw  = top ? 1 : widths[static_cast<std::size_t>(level + 1)];
      const int  ch  = top ? 1 : heights[static_cast<std::size_t>(level + 1)];
      auto&      out = result[static_cast<std::size_t>(level)];
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x);
          const float value = source[static_cast<std::size_t>(level)][index];
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
            out[index] = remap_a[static_cast<std::size_t>(level)][index] +
                         (remap_b[static_cast<std::size_t>(level)][index] -
                          remap_a[static_cast<std::size_t>(level)][index]) *
                             t;
            continue;
          }
          const float lap_lo = remap_a[static_cast<std::size_t>(level)][index] -
                               expand(remap_a[static_cast<std::size_t>(level + 1)], cw, ch, x, y);
          const float lap_hi = remap_b[static_cast<std::size_t>(level)][index] -
                               expand(remap_b[static_cast<std::size_t>(level + 1)], cw, ch, x, y);
          out[index] = lap_lo + (lap_hi - lap_lo) * t;
        }
      }
    }
    if (pair + 2 < samples.size()) {
      remap_a.swap(remap_b);
      remap_b = build_remap(samples[pair + 2]);
    }
  }
  for (int level = count - 2; level >= 0; --level) {
    const int w         = widths[static_cast<std::size_t>(level)];
    const int h         = heights[static_cast<std::size_t>(level)];
    const int cw        = widths[static_cast<std::size_t>(level + 1)];
    const int ch        = heights[static_cast<std::size_t>(level + 1)];
    auto      collapsed = make_plane(level);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const auto index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        collapsed[index] = result[static_cast<std::size_t>(level)][index] +
                           expand(result[static_cast<std::size_t>(level + 1)], cw, ch, x, y);
      }
    }
    result[static_cast<std::size_t>(level)] = std::move(collapsed);
  }

  auto bilinear = [](const std::vector<float>& plane, int w, int h, float x, float y) {
    x              = std::clamp(x, 0.0f, static_cast<float>(w - 1));
    y              = std::clamp(y, 0.0f, static_cast<float>(h - 1));
    const int   x0 = static_cast<int>(std::floor(x));
    const int   y0 = static_cast<int>(std::floor(y));
    const int   x1 = std::min(x0 + 1, w - 1);
    const int   y1 = std::min(y0 + 1, h - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float a  = plane[static_cast<std::size_t>(y0) * static_cast<std::size_t>(w) +
                          static_cast<std::size_t>(x0)] +
                    (plane[static_cast<std::size_t>(y0) * static_cast<std::size_t>(w) +
                           static_cast<std::size_t>(x1)] -
                     plane[static_cast<std::size_t>(y0) * static_cast<std::size_t>(w) +
                           static_cast<std::size_t>(x0)]) *
                        tx;
    const float b = plane[static_cast<std::size_t>(y1) * static_cast<std::size_t>(w) +
                          static_cast<std::size_t>(x0)] +
                    (plane[static_cast<std::size_t>(y1) * static_cast<std::size_t>(w) +
                           static_cast<std::size_t>(x1)] -
                     plane[static_cast<std::size_t>(y1) * static_cast<std::size_t>(w) +
                           static_cast<std::size_t>(x0)]) *
                        tx;
    return a + (b - a) * ty;
  };

  std::vector<Rgba> output = input;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float ax = (static_cast<float>(x) + 0.5f) * static_cast<float>(mask.width) /
                           static_cast<float>(width) -
                       0.5f;
      const float ay = (static_cast<float>(y) + 0.5f) * static_cast<float>(mask.height) /
                           static_cast<float>(height) -
                       0.5f;
      const auto      index            = static_cast<std::size_t>(y) * width + x;
      const float     reference_l      = bilinear(source[0], mask.width, mask.height, ax, ay);
      const float     adjusted_l       = bilinear(result[0], mask.width, mask.height, ax, ay);
      const auto&     pixel            = input[index];
      const float     source_l         = LogIntensity(pixel);
      const float     source_intensity = std::max(AcesccDecode(source_l), 1.0e-5f);
      const float     target_intensity = AcesccDecode(source_l + adjusted_l - reference_l);
      const float     ratio  = std::clamp(target_intensity / source_intensity, 0.0f, 32.0f);
      float           r      = AcesccDecode(pixel.r) * ratio;
      float           g      = AcesccDecode(pixel.g) * ratio;
      float           b      = AcesccDecode(pixel.b) * ratio;
      constexpr float kLower = -1.0e-5f;
      float           gamut  = 1.0f;
      if (r < kLower && target_intensity > r) {
        gamut = std::min(gamut, (target_intensity - kLower) / (target_intensity - r));
      }
      if (g < kLower && target_intensity > g) {
        gamut = std::min(gamut, (target_intensity - kLower) / (target_intensity - g));
      }
      if (b < kLower && target_intensity > b) {
        gamut = std::min(gamut, (target_intensity - kLower) / (target_intensity - b));
      }
      gamut         = std::clamp(gamut, 0.0f, 1.0f);
      r             = target_intensity + (r - target_intensity) * gamut;
      g             = target_intensity + (g - target_intensity) * gamut;
      b             = target_intensity + (b - target_intensity) * gamut;
      output[index] = {AcesccEncode(r), AcesccEncode(g), AcesccEncode(b), pixel.a};
    }
  }
  return output;
}

class MetalLlfFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                              gpu_dag_test::FullSensor(16, 12));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), RenderRequest{});
  }

  auto RenderGrade() -> MetalPrimaryGradeResult {
    return RenderPreparedGrade(device_, document_, prepared_, plan_);
  }

  template <class Model>
  auto ModelByType(const OperatorTypeId& type) -> Model& {
    auto* model = dynamic_cast<Model*>(document_.PrimaryGrade()->FindAdjustmentByType(type));
    EXPECT_NE(model, nullptr);
    return *model;
  }

  PreparedRawInput  prepared_;
  PipelineDocument  document_;
  ExecutionPlan     plan_;
  MetalRenderDevice device_;
};

TEST_F(MetalLlfFixture, MetalLlfDestroysPyramidScratchAfterCompletedRender) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto result = RenderGrade();
  EXPECT_GT(result.local_tone_transient_bytes, 0U);
  EXPECT_NE(device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                              PortId{"local_tone.source.0"}),
            nullptr);
  EXPECT_NE(device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                              PortId{"local_tone.result.0"}),
            nullptr);
  EXPECT_EQ(device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                              PortId{"local_tone.source.1"}),
            nullptr);
  EXPECT_EQ(device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                              PortId{"local_tone.remap_a.0"}),
            nullptr);
  EXPECT_EQ(device_.Workspace().TransientBuffers().used_bytes(), 0U);
  EXPECT_EQ(device_.Workspace().Device().RecordedWorkScratchBufferCount(), 0U);
  EXPECT_GE(plan_.peak_transient_bytes, result.local_tone_transient_bytes);
}

TEST_F(MetalLlfFixture, MetalLlfFullFrameBuildsCanonicalReferenceOnce) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto first  = RenderGrade();
  const auto second = RenderGrade();
  EXPECT_TRUE(first.local_tone_rebuilt_reference);
  EXPECT_FALSE(first.local_tone_sampled_canonical_reference);
  EXPECT_FALSE(second.local_tone_rebuilt_reference);
  EXPECT_TRUE(second.local_tone_sampled_canonical_reference);
  EXPECT_EQ(first.local_tone_reference_resource_id, second.local_tone_reference_resource_id);
  EXPECT_NE(first.local_tone_reference_resource_id, 0U);
}

TEST_F(MetalLlfFixture, MetalLlfSliderEditReusesCanonicalReference) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto first = RenderGrade();
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(60.0f);
  device_.Workspace().Device().ResetCounters();
  const auto second = RenderGrade();
  EXPECT_EQ(first.local_tone_reference_resource_id, second.local_tone_reference_resource_id);
  EXPECT_TRUE(second.local_tone_rebuilt_reference);
  EXPECT_FALSE(second.local_tone_sampled_canonical_reference);
  EXPECT_GT(device_.Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_GT(device_.Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().FreeCount(),
            device_.Workspace().Device().BufferCreateCount() +
                device_.Workspace().Device().TextureCreateCount());
  EXPECT_EQ(device_.Workspace().Device().PipelineCreateCount(), 0U);
}

TEST_F(MetalLlfFixture, MetalLlfSecondStableRenderRecreatesOnlyDisposableScratch) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  (void)RenderGrade();
  device_.Workspace().Device().ResetCounters();
  (void)RenderGrade();
  EXPECT_EQ(device_.Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_GT(device_.Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().FreeCount(),
            device_.Workspace().Device().TextureCreateCount());
  EXPECT_EQ(device_.Workspace().Device().RecordedWorkScratchBufferCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().RecordedWorkScratchTextureCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().HeapCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().PipelineCreateCount(), 0U);
}

TEST_F(MetalLlfFixture, MetalLlfFailedSubmissionDoesNotPublishReference) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto first = RenderGrade();
  EXPECT_NE(first.local_tone_reference_resource_id, 0U);
  const auto* source = device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                                         PortId{"local_tone.source.0"});
  ASSERT_NE(source, nullptr);
  const auto source_id = source->ResourceId();

  device_.BeginRender();
  ExecuteMetalDevelop(device_, plan_, prepared_, document_);
  ExecuteMetalGeometryResample(device_, plan_);
  ExecuteMetalCameraColor(device_, plan_, document_);
  plan_.geometry.full_reference_extent = {};
  EXPECT_THROW((void)ExecuteMetalPrimaryGrade(device_, plan_, prepared_, document_),
               std::runtime_error);
  device_.CancelRender();
  const auto* after = device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                                        PortId{"local_tone.source.0"});
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->ResourceId(), source_id);

  plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), RenderRequest{});
  const auto recovered = RenderGrade();
  EXPECT_TRUE(recovered.local_tone_sampled_canonical_reference);
  EXPECT_EQ(recovered.local_tone_reference_resource_id, source_id);
}

TEST(GpuDagMetalGrade, MetalLlfRoiSamplesCanonicalReferenceWithSharedGeometryPlan) {
  if (!HasMetalDevice()) {
    GTEST_SKIP() << "No Metal device available.";
  }
  (void)BindSystemDefaultMetalPresentationDevice();
  constexpr std::uint32_t kWidth  = 64;
  constexpr std::uint32_t kHeight = 64;
  auto prepared = RawInputLoader::FromDirectRgb(MakeSplitPlane(kWidth, kHeight, 1.0f, 0.05f),
                                                gpu_dag_test::FullSensor(kWidth, kHeight));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(80.0f);

  auto full_plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  const auto        full_grade = RenderPreparedGrade(device, document, prepared, full_plan);
  EXPECT_TRUE(full_grade.local_tone_rebuilt_reference);
  EXPECT_FALSE(full_grade.local_tone_sampled_canonical_reference);
  const auto full_pixels = DownloadRgba(device, full_grade.output);
  const auto full_mask =
      DownloadPlane(device, document.PrimaryGrade()->Id(), "local_tone.source.0");
  ASSERT_FALSE(full_pixels.empty());
  ASSERT_FALSE(full_mask.empty());

  RenderRequest roi_request;
  roi_request.view.visible_rect_in_edit_space = {0.5f, 0.0f, 0.5f, 1.0f};
  auto       roi_plan  = GraphCompiler::Compile(document, prepared.CompileSource(), roi_request);
  const auto roi_grade = RenderPreparedGrade(device, document, prepared, roi_plan);
  EXPECT_FALSE(roi_grade.local_tone_rebuilt_reference);
  EXPECT_TRUE(roi_grade.local_tone_sampled_canonical_reference);
  EXPECT_EQ(full_grade.local_tone_reference_resource_id,
            roi_grade.local_tone_reference_resource_id);
  const auto roi_pixels = DownloadRgba(device, roi_grade.output);
  const auto roi_mask = DownloadPlane(device, document.PrimaryGrade()->Id(), "local_tone.source.0");
  ASSERT_FALSE(roi_pixels.empty());
  ASSERT_EQ(full_mask, roi_mask);

  const auto full_probe =
      TransformPoint(full_plan.geometry.render_to_reference, PixelCenter(48, 32));
  const auto roi_probe = TransformPoint(roi_plan.geometry.reference_to_render, full_probe);
  const auto roi_x     = static_cast<int>(std::floor(roi_probe.x));
  const auto roi_y     = static_cast<int>(std::floor(roi_probe.y));
  const auto roi_w     = static_cast<int>(roi_plan.geometry.render_extent.width);
  const auto roi_h     = static_cast<int>(roi_plan.geometry.render_extent.height);
  ASSERT_GE(roi_x, 0);
  ASSERT_GE(roi_y, 0);
  ASSERT_LT(roi_x, roi_w);
  ASSERT_LT(roi_y, roi_h);
  const float sampled = roi_pixels[static_cast<std::size_t>(roi_y) * roi_w + roi_x].r;
  const float full    = full_pixels[static_cast<std::size_t>(32) * kWidth + 48].r;
  EXPECT_NEAR(sampled, full, 2.0e-3f);

  MetalRenderDevice isolated;
  const auto        isolated_grade = RenderPreparedGrade(isolated, document, prepared, roi_plan);
  EXPECT_TRUE(isolated_grade.local_tone_rebuilt_reference);
  EXPECT_FALSE(isolated_grade.local_tone_sampled_canonical_reference);
  const auto isolated_pixels = DownloadRgba(isolated, isolated_grade.output);
  ASSERT_FALSE(isolated_pixels.empty());
  const float rebuilt = isolated_pixels[static_cast<std::size_t>(roi_y) * roi_w + roi_x].r;
  EXPECT_GT(std::abs(rebuilt - sampled), 1.0e-4f);
}

TEST(GpuDagMetalGrade, MetalLlfMatchesCudaReferenceWithinTolerance) {
  if (!HasMetalDevice()) {
    GTEST_SKIP() << "No Metal device available.";
  }
  (void)BindSystemDefaultMetalPresentationDevice();
  constexpr std::uint32_t kWidth  = 32;
  constexpr std::uint32_t kHeight = 32;
  auto prepared = RawInputLoader::FromDirectRgb(MakeSplitPlane(kWidth, kHeight, 0.08f, 0.45f),
                                                gpu_dag_test::FullSensor(kWidth, kHeight));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(80.0f);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  const auto        result = RenderPreparedGrade(device, document, prepared, plan);
  const auto        input  = DownloadRgba(device, plan.develop_output);
  const auto        output = DownloadRgba(device, result.output);
  ASSERT_EQ(input.size(), output.size());
  const auto cpu = CpuApplyLlf(input, kWidth, kHeight, 80.0f, 0.0f);
  ASSERT_EQ(cpu.size(), output.size());
  float max_err = 0.0f;
  for (std::size_t i = 0; i < output.size(); ++i) {
    max_err = std::max(max_err, std::fabs(cpu[i].r - output[i].r));
    max_err = std::max(max_err, std::fabs(cpu[i].g - output[i].g));
    max_err = std::max(max_err, std::fabs(cpu[i].b - output[i].b));
  }
  EXPECT_LT(max_err, 2.0e-3f);

  auto dark_prepared   = RawInputLoader::FromDirectRgb(MakeNeighborhoodPlane(64, 64, 0.02f, 0.08f),
                                                       gpu_dag_test::FullSensor(64, 64));
  auto bright_prepared = RawInputLoader::FromDirectRgb(MakeNeighborhoodPlane(64, 64, 0.45f, 0.08f),
                                                       gpu_dag_test::FullSensor(64, 64));
  auto dark_document   = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(dark_document);
  dynamic_cast<ShadowsModel*>(
      dark_document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()))
      ->SetValue(80.0f);
  auto bright_document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(bright_document);
  dynamic_cast<HighlightsModel*>(
      bright_document.PrimaryGrade()->FindAdjustmentByType(type_ids::Highlights()))
      ->SetValue(0.0f);
  dynamic_cast<ShadowsModel*>(
      bright_document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()))
      ->SetValue(80.0f);
  MetalRenderDevice dark_device;
  MetalRenderDevice bright_device;
  auto              dark_plan =
      GraphCompiler::Compile(dark_document, dark_prepared.CompileSource(), RenderRequest{});
  auto bright_plan =
      GraphCompiler::Compile(bright_document, bright_prepared.CompileSource(), RenderRequest{});
  const auto dark_out = DownloadRgba(
      dark_device,
      RenderPreparedGrade(dark_device, dark_document, dark_prepared, dark_plan).output);
  const auto bright_out = DownloadRgba(
      bright_device,
      RenderPreparedGrade(bright_device, bright_document, bright_prepared, bright_plan).output);
  ASSERT_FALSE(dark_out.empty());
  ASSERT_FALSE(bright_out.empty());
  const auto center = static_cast<std::size_t>(32) * 64 + 32;
  EXPECT_GT(std::abs(dark_out[center].r - bright_out[center].r), 1.0e-4f);
}

TEST_F(MetalLlfFixture, MetalLocalToneMissingMetallibThrowsExplicitError) {
  EXPECT_THROW(
      (void)metal::ComputePipelineCache::Instance().GetPipelineState(
          "/alcedo/missing/local_tone.metallib", "local_tone_extract", "Metal LLF extract"),
      std::runtime_error);
}

}  // namespace
}  // namespace alcedo
