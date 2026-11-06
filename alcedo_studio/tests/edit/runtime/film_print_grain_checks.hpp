//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// The checks of the film print with the grain in its negative, through the DRT pass, on every
// backend (plan 'film_grain_format_and_stock_plan.md', phase G5 on CUDA, phase G6 on OpenCL and
// Metal). A backend test gives a render function that runs its DRT pass on a flat scene; the checks
// are the same for each backend. No stored pixels.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "film_grain_post_checks.hpp"

namespace alcedo::film_print_grain_test {

struct Rgba {
  float r;
  float g;
  float b;
  float a;
};

/// The output of one render of the DRT pass, and the counts of the render.
struct Frame {
  std::vector<Rgba> pixels;
  std::uint64_t     grain_negative_passes = 0;  ///< `film_grain_negative` of the pass stats
  std::uint32_t     post_count            = 0;  ///< `post_neighborhood_count` of the result
};

/**
 * Runs the DRT pass of one backend with @p plan on a flat scene of the linear AP1 grey @p grey,
 * with the render size of `plan.geometry`, and resets the pass stats first. `plan` holds the
 * compiled DRT/Post adjustments and `film_scan_linear_output`.
 */
using Render =
    std::function<Frame(const ExecutionPlan& plan, const PipelineDocument& document, float grey)>;

/// A film print document with Film Grain at @p strength and the film format @p format.
inline auto Document(const std::string& negative, const std::string& print_material, float strength,
                     FilmFormat format = FilmFormat::Film35mm) -> PipelineDocument {
  auto               document = CreateDefaultPipelineDocument();
  DrtParameterUpdate update;
  update.method              = DrtMethod::FilmPrint;
  update.encoding_space      = DrtColorSpace::Rec709;
  update.encoding_eotf       = DrtEotf::SrgbPiecewise;
  update.film_negative       = negative;
  update.film_print_material = print_material;
  update.film_scene_light    = "reference";
  document.Drt()->Params().ApplyUpdate(update);
  document.Drt()->Params().ApplyFilmFormat(format);
  auto* grain =
      dynamic_cast<FilmGrainModel*>(document.Drt()->FindAdjustmentByType(type_ids::FilmGrain()));
  if (grain == nullptr) throw std::runtime_error("The document has no Film Grain");
  grain->SetValue(strength);
  return document;
}

inline auto StoredStrength(const PipelineDocument& document) -> float {
  const auto* grain = dynamic_cast<const FilmGrainModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::FilmGrain()));
  if (grain == nullptr) throw std::runtime_error("The document has no Film Grain");
  return grain->Read([](const ScalarFloatPayload& payload) { return payload.value; });
}

/**
 * The plan of a render of @p size x @p size pixels at reference pixel (700, 500) of a 6000 x 4000
 * frame. With @p compile_post, it compiles the DRT/Post adjustments of @p document, as
 * GraphCompiler lists them. @p linear_scan gives the linear AP1 of Negative Scan before its
 * display transform.
 */
inline auto Plan(const PipelineDocument& document, std::uint32_t size, bool linear_scan = false,
                 bool compile_post = true) -> ExecutionPlan {
  ExecutionPlan plan;
  auto&         geometry         = plan.geometry;
  geometry.full_reference_extent = {6000, 4000};
  geometry.edit_extent           = geometry.full_reference_extent;
  geometry.render_extent         = {size, size};
  geometry.reference_to_edit     = Matrix3x3::Identity();
  geometry.render_to_reference   = Matrix3x3::Translate(700.0f, 500.0f);
  geometry.reference_to_render   = InvertAffine(geometry.render_to_reference);
  geometry.edit_to_render        = geometry.reference_to_render;
  plan.film_scan_linear_output   = linear_scan;
  if (compile_post) {
    const auto& drt = *document.Drt();
    for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
      plan.drt.post_adjustments.push_back({drt.AdjustmentIdAt(index),
                                           drt.AdjustmentAt(index).Type(),
                                           CompiledAdjustmentAlgorithm::Neighborhood});
    }
  }
  return plan;
}

/// ln of the linear light of one channel: of the sRGB display, or of linear AP1.
inline auto LogLight(const std::vector<Rgba>& pixels, int channel, bool linear)
    -> std::vector<double> {
  std::vector<double> values(pixels.size());
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const float v = channel == 0 ? pixels[i].r : (channel == 1 ? pixels[i].g : pixels[i].b);
    values[i]     = std::log(static_cast<double>(linear ? v : CeSrgbDecode(v)));
  }
  return values;
}

inline auto MeanLight(const std::vector<Rgba>& pixels, int channel) -> double {
  double sum = 0.0;
  for (const auto& pixel : pixels) {
    sum += CeSrgbDecode(channel == 0 ? pixel.r : (channel == 1 ? pixel.g : pixel.b));
  }
  return sum / static_cast<double>(pixels.size());
}

// The plan: through Negative Scan of 5207, the channel correlation rg is within 0.05 of 0.661 (the
// 5207 scans of #660). The lab fitted the layer correlation R at net green density 0.1 to 0.7 of
// the default scanner (P15 `fit_layer_correlation.py`); these greys cover that range.
inline void ExpectNegativeScanChannelCorrelationIsTheScanCorrelationOf660(const Render& render) {
  const auto            document = Document("kodak_5207", std::string{kFilmNegativeScanId}, 1.0f);
  const auto            plan     = Plan(document, 512, true);
  std::array<double, 3> mean{};
  const std::array<float, 4> greys{0.03f, 0.09f, 0.25f, 0.7f};
  for (const float grey : greys) {
    const auto frame = render(plan, document, grey);
    EXPECT_EQ(frame.grain_negative_passes, 1U);
    const auto                  r = LogLight(frame.pixels, 0, true);
    const auto                  g = LogLight(frame.pixels, 1, true);
    const auto                  b = LogLight(frame.pixels, 2, true);
    const std::array<double, 3> correlation{film_grain_post_test::Correlation(r, g),
                                            film_grain_post_test::Correlation(g, b),
                                            film_grain_post_test::Correlation(r, b)};
    std::cout << "Negative Scan of 5207, AP1 grey " << grey << ": rg " << correlation[0] << ", gb "
              << correlation[1] << ", rb " << correlation[2] << "\n";
    for (std::size_t pair = 0; pair < 3; ++pair) mean[pair] += correlation[pair] / greys.size();
  }
  EXPECT_NEAR(mean[0], 0.661, 0.05);
  EXPECT_NEAR(mean[1], 0.41, 0.05);
  EXPECT_NEAR(mean[2], 0.246, 0.05);
}

// The plan: with strength 0 the output equals the film print without grain, and no grain kernel
// runs. In each form of FP6: the print, Negative Scan, and the light table.
inline void ExpectStrengthZeroEqualsTheFilmPrintWithNoGrainKernel(const Render& render) {
  struct Form {
    const char* negative;
    std::string print_material;
  };
  for (const auto& form :
       {Form{"kodak_5207", "kodak_2383"}, Form{"kodak_5207", std::string{kFilmNegativeScanId}},
        Form{"fuji_velvia_50", "kodak_2383"}}) {
    SCOPED_TRACE(std::string(form.negative) + " " + form.print_material);
    const auto zero  = Document(form.negative, form.print_material, 0.0f);
    const auto plain = render(Plan(zero, 96), zero, 0.18f);
    EXPECT_EQ(plain.grain_negative_passes, 0U);
    // The same document with no compiled DRT/Post adjustment: the film print of F4 and F10.
    const auto none = render(Plan(zero, 96, false, false), zero, 0.18f);
    ASSERT_EQ(plain.pixels.size(), none.pixels.size());
    for (std::size_t i = 0; i < plain.pixels.size(); ++i) {
      ASSERT_EQ(plain.pixels[i].r, none.pixels[i].r) << i;
      ASSERT_EQ(plain.pixels[i].g, none.pixels[i].g) << i;
      ASSERT_EQ(plain.pixels[i].b, none.pixels[i].b) << i;
    }
    const auto grain = Document(form.negative, form.print_material, 1.0f);
    const auto with  = render(Plan(grain, 96), grain, 0.18f);
    EXPECT_EQ(with.grain_negative_passes, 1U);
    bool different = false;
    for (std::size_t i = 0; i < with.pixels.size(); ++i) {
      different = different || with.pixels[i].r != plain.pixels[i].r ||
                  with.pixels[i].g != plain.pixels[i].g;
    }
    EXPECT_TRUE(different);
  }
}

// Decision U4: the film print skips the DRT/Post Film Grain and keeps its stored value; the grain
// of the print grows with the slider.
inline void ExpectFilmPrintSkipsThePostFilmGrainAndKeepsItsStoredValue(const Render& render) {
  for (const auto* negative : {"kodak_5207", "fuji_velvia_50"}) {
    SCOPED_TRACE(negative);
    const auto half       = Document(negative, "kodak_2383", 0.5f);
    const auto full       = Document(negative, "kodak_2383", 1.0f);
    const auto half_frame = render(Plan(half, 256), half, 0.18f);
    EXPECT_EQ(half_frame.post_count, 0U);
    const auto full_frame = render(Plan(full, 256), full, 0.18f);
    EXPECT_EQ(full_frame.post_count, 0U);
    EXPECT_EQ(StoredStrength(full), 1.0f);
    for (int channel = 0; channel < 3; ++channel) {
      SCOPED_TRACE(channel);
      const double half_sigma =
          film_grain_post_test::StdDev(LogLight(half_frame.pixels, channel, false));
      const double full_sigma =
          film_grain_post_test::StdDev(LogLight(full_frame.pixels, channel, false));
      EXPECT_GT(half_sigma, 0.0);
      EXPECT_NEAR(full_sigma / half_sigma, 2.0, 0.2);
    }
  }
}

// Risk "Grain in the negative moves the mean through the print curve" (section 10): the change of
// the mean display light of a flat grey with the grain of 5207 at 100 %, in stops, at 6000 px. The
// print turns a density change into light through a convex curve, so the grain makes the mean
// light of a print shadow brighter, as on a real print; the mid tones and the highlights keep their
// mean. The record is printed for the plan; each bound is the measured largest value of CUDA with a
// margin, so a change of the model shows here.
inline void ExpectGrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows(const Render& render) {
  struct Bound {
    FilmFormat format;
    double     shadows;  ///< AP1 grey below 0.18
    double     rest;     ///< AP1 grey 0.18 and above
  };
  for (const auto& bound :
       {Bound{FilmFormat::Film35mm, 0.2, 0.06}, Bound{FilmFormat::Film16mm, 0.35, 0.1}}) {
    const auto zero = Document("kodak_5207", "kodak_2383", 0.0f, bound.format);
    const auto full = Document("kodak_5207", "kodak_2383", 1.0f, bound.format);
    for (const float grey : {0.005f, 0.02f, 0.06f, 0.18f, 0.5f, 1.5f, 4.0f}) {
      const auto plain = render(Plan(zero, 256), zero, grey).pixels;
      const auto grain = render(Plan(full, 256), full, grey).pixels;
      std::cout << "5207 on 2383, " << FilmFormatId(bound.format) << ", AP1 grey " << grey
                << ": mean shift";
      for (int channel = 0; channel < 3; ++channel) {
        const double stops = std::log2(MeanLight(grain, channel) / MeanLight(plain, channel));
        std::cout << " " << stops;
        EXPECT_LT(std::abs(stops), grey < 0.18f ? bound.shadows : bound.rest)
            << FilmFormatId(bound.format) << " grey " << grey << " channel " << channel;
      }
      std::cout << " stops\n";
    }
  }
}

}  // namespace alcedo::film_print_grain_test
