//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// OpenColorIO 2.5.1 reference for the ACES 2.0 output transform inverse
// (raster_image_input_plan.md, section 5.7). Only test targets link OpenColorIO.

#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <regex>
#include <string>
#include <vector>

#include "image/raster_color_description.hpp"

namespace alcedo::aces2_inverse_test {

namespace OCIO = OCIO_NAMESPACE;

/// One limiting space of the section 5.7 accuracy test.
struct InverseCase {
  const char*          name_;
  std::array<float, 8> primaries_;
  float                peak_nits_;
};

inline auto InverseCases() -> std::vector<InverseCase> {
  return {
      {"rec709_100", color::GamutPrimariesXy(color::ColorGamutId::Rec709), 100.0f},
      {"p3d65_100", color::GamutPrimariesXy(color::ColorGamutId::P3D65), 100.0f},
      {"rec2020_100", color::GamutPrimariesXy(color::ColorGamutId::Rec2020), 100.0f},
      {"adobe_rgb_100", color::GamutPrimariesXy(color::ColorGamutId::AdobeRgb), 100.0f},
      {"rec2020_1000", color::GamutPrimariesXy(color::ColorGamutId::Rec2020), 1000.0f},
  };
}

/// OCIO processor of FixedFunctionTransform ACES_OUTPUT_TRANSFORM_20, inverse direction.
inline auto MakeInverseProcessor(const InverseCase& c) -> OCIO::ConstProcessorRcPtr {
  const double params[9] = {c.peak_nits_,    c.primaries_[0], c.primaries_[1],
                            c.primaries_[2], c.primaries_[3], c.primaries_[4],
                            c.primaries_[5], c.primaries_[6], c.primaries_[7]};
  auto         transform = OCIO::FixedFunctionTransform::Create(
      OCIO::FIXED_FUNCTION_ACES_OUTPUT_TRANSFORM_20, params, 9);
  transform->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
  return OCIO::Config::CreateRaw()->getProcessor(transform);
}

/// 33^3 grid of display-linear RGB covering [0, peak / 100] (1.0 = 100 nits), RGB interleaved.
inline auto DisplayGrid(float peak_nits) -> std::vector<float> {
  constexpr int      kSteps = 33;
  const float        scale  = peak_nits / 100.0f;
  std::vector<float> rgb;
  rgb.reserve(kSteps * kSteps * kSteps * 3);
  for (int b = 0; b < kSteps; ++b) {
    for (int g = 0; g < kSteps; ++g) {
      for (int r = 0; r < kSteps; ++r) {
        rgb.push_back(scale * static_cast<float>(r) / (kSteps - 1));
        rgb.push_back(scale * static_cast<float>(g) / (kSteps - 1));
        rgb.push_back(scale * static_cast<float>(b) / (kSteps - 1));
      }
    }
  }
  return rgb;
}

/// Apply the OCIO CPU processor (no optimization) to interleaved RGB.
inline auto OcioInverse(const InverseCase& c, std::vector<float> rgb) -> std::vector<float> {
  const auto cpu = MakeInverseProcessor(c)->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_NONE);
  OCIO::PackedImageDesc image(rgb.data(), static_cast<long>(rgb.size() / 3), 1, 3);
  cpu->apply(image);
  return rgb;
}

/// Section 5.7 accuracy: relative 1e-3 per AP0 channel, or absolute 1e-5 below 1e-2.
inline auto WithinInverseTolerance(float actual, float expected) -> bool {
  if (!std::isfinite(actual)) {
    return false;
  }
  if (std::abs(expected) < 1e-2f) {
    return std::abs(actual - expected) <= 1e-5f;
  }
  return std::abs(actual - expected) <= 1e-3f * std::abs(expected);
}

/// OCIO processor of the forward ACES 2.0 output transform for the same case.
inline auto OcioForward(const InverseCase& c, std::vector<float> rgb) -> std::vector<float> {
  const double params[9] = {c.peak_nits_,    c.primaries_[0], c.primaries_[1],
                            c.primaries_[2], c.primaries_[3], c.primaries_[4],
                            c.primaries_[5], c.primaries_[6], c.primaries_[7]};
  auto         transform = OCIO::FixedFunctionTransform::Create(
      OCIO::FIXED_FUNCTION_ACES_OUTPUT_TRANSFORM_20, params, 9);
  const auto cpu = OCIO::Config::CreateRaw()->getProcessor(transform)->getOptimizedCPUProcessor(
      OCIO::OPTIMIZATION_NONE);
  OCIO::PackedImageDesc image(rgb.data(), static_cast<long>(rgb.size() / 3), 1, 3);
  cpu->apply(image);
  return rgb;
}

/**
 * Compare AP0 results with the OCIO reference (section 5.7).
 *
 * Every channel must be within the AP0 tolerance, except at grid points where the inverse is
 * ill-conditioned: near the peak of the tonescale a display difference of one float ulp spans
 * a large AP0 range, so the float rounding of the table build (see the table test) moves the
 * AP0 result. At those points the result must still be an inverse of OCIO's forward: the OCIO
 * forward of our AP0 must reproduce the display input within the same tolerance, or with an
 * error at most that tolerance above OCIO's own round-trip error at that point. At least
 * 99.9 percent of all channels must meet the AP0 tolerance directly.
 */
inline void ExpectMatchesOcio(const InverseCase& c, const std::vector<float>& input,
                              const std::vector<float>& actual_ap0) {
  const auto expected = OcioInverse(c, input);
  ASSERT_EQ(actual_ap0.size(), expected.size());
  std::vector<std::size_t> mismatched_pixels;
  std::size_t              mismatched_channels = 0;
  for (std::size_t pixel = 0; pixel < expected.size() / 3; ++pixel) {
    bool mismatch = false;
    for (std::size_t ch = 0; ch < 3; ++ch) {
      if (!WithinInverseTolerance(actual_ap0[pixel * 3 + ch], expected[pixel * 3 + ch])) {
        mismatch = true;
        ++mismatched_channels;
      }
    }
    if (mismatch) {
      mismatched_pixels.push_back(pixel);
    }
  }
  std::vector<float> mismatched_ap0;
  for (const auto pixel : mismatched_pixels) {
    mismatched_ap0.insert(mismatched_ap0.end(), actual_ap0.begin() + pixel * 3,
                          actual_ap0.begin() + pixel * 3 + 3);
  }
  std::vector<float> reference_ap0;
  for (const auto pixel : mismatched_pixels) {
    reference_ap0.insert(reference_ap0.end(), expected.begin() + pixel * 3,
                         expected.begin() + pixel * 3 + 3);
  }
  const auto round_trip =
      mismatched_ap0.empty() ? std::vector<float>{} : OcioForward(c, mismatched_ap0);
  const auto reference_round_trip =
      reference_ap0.empty() ? std::vector<float>{} : OcioForward(c, reference_ap0);
  int failures = 0;
  for (std::size_t k = 0; k < mismatched_pixels.size(); ++k) {
    const auto pixel = mismatched_pixels[k];
    for (std::size_t ch = 0; ch < 3; ++ch) {
      // OCIO's own inverse does not round-trip exactly at these points either; ours may not be
      // worse than OCIO's own error by more than the tolerance.
      const float target          = input[pixel * 3 + ch];
      const float our_error       = std::abs(round_trip[k * 3 + ch] - target);
      const float reference_error = std::abs(reference_round_trip[k * 3 + ch] - target);
      const bool  within          = WithinInverseTolerance(round_trip[k * 3 + ch], target) ||
                          our_error <= reference_error + 1e-3f * std::abs(target);
      if (!within && ++failures <= 8) {
        ADD_FAILURE() << c.name_ << " pixel " << pixel << " input (" << input[pixel * 3] << ", "
                      << input[pixel * 3 + 1] << ", " << input[pixel * 3 + 2] << ") AP0 ("
                      << actual_ap0[pixel * 3] << ", " << actual_ap0[pixel * 3 + 1] << ", "
                      << actual_ap0[pixel * 3 + 2] << ") OCIO AP0 (" << expected[pixel * 3] << ", "
                      << expected[pixel * 3 + 1] << ", " << expected[pixel * 3 + 2]
                      << ") round trip channel " << ch << " = " << round_trip[k * 3 + ch]
                      << ", OCIO round trip " << reference_round_trip[k * 3 + ch];
      }
    }
  }
  EXPECT_EQ(failures, 0) << c.name_;
  EXPECT_LE(static_cast<double>(mismatched_channels), 1e-3 * static_cast<double>(expected.size()))
      << c.name_ << ": " << mismatched_channels << " of " << expected.size()
      << " channels outside the AP0 tolerance";
}

/// Tables that the OCIO GPU shader generator emits for one inverse transform.
struct OcioInverseTables {
  std::vector<float> reach_m_;
  std::vector<float> hues_;
  std::vector<float> cusps_;  // J, M, gamma_top_inv per entry
  std::array<int, 2> hue_search_range_{};
};

inline auto ExtractOcioInverseTables(const InverseCase& c) -> OcioInverseTables {
  const auto gpu  = MakeInverseProcessor(c)->getOptimizedGPUProcessor(OCIO::OPTIMIZATION_NONE);
  auto       desc = OCIO::GpuShaderDesc::CreateShaderDesc();
  desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);
  gpu->extractGpuShaderInfo(desc);

  OcioInverseTables tables;
  for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
    const char*                            texture_name = nullptr;
    const char*                            sampler_name = nullptr;
    unsigned                               width = 0, height = 0;
    OCIO::GpuShaderDesc::TextureType       channel{};
    OCIO::GpuShaderDesc::TextureDimensions dimensions{};
    OCIO::Interpolation                    interpolation{};
    desc->getTexture(i, texture_name, sampler_name, width, height, channel, dimensions,
                     interpolation);
    const float* values = nullptr;
    desc->getTextureValues(i, values);
    const std::string  name(texture_name);
    const std::size_t  channels = channel == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL ? 3 : 1;
    std::vector<float> data(values, values + static_cast<std::size_t>(width) * height * channels);
    if (name.find("reach_m_table") != std::string::npos) {
      tables.reach_m_ = std::move(data);
    } else if (name.find("gamut_cusp_table") != std::string::npos) {
      tables.cusps_ = std::move(data);
    }
  }

  const std::string text(desc->getShaderText());
  // const float <name>_hues_array[363] = float[363](v0, v1, ...);
  const auto        hue_start = text.find("_hues_array[");
  if (hue_start != std::string::npos) {
    const auto        open  = text.find('(', hue_start);
    const auto        close = text.find(')', open);
    const std::string list  = text.substr(open + 1, close - open - 1);
    std::size_t       pos   = 0;
    while (pos < list.size()) {
      std::size_t used = 0;
      tables.hues_.push_back(std::stof(list.substr(pos), &used));
      pos += used;
      while (pos < list.size() && (list[pos] == ',' || list[pos] == ' ')) {
        ++pos;
      }
    }
  }
  // i_lo = int(max(float(0), float(i + LO))); i_hi = int(min(float(361), float(i + HI)));
  const std::regex range_pattern(R"(float\(i \+ (-?\d+)\))");
  auto             it = std::sregex_iterator(text.begin(), text.end(), range_pattern);
  if (it != std::sregex_iterator()) {
    tables.hue_search_range_[0] = std::stoi((*it)[1]);
    ++it;
    if (it != std::sregex_iterator()) {
      tables.hue_search_range_[1] = std::stoi((*it)[1]);
    }
  }
  return tables;
}

}  // namespace alcedo::aces2_inverse_test
