//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film_grain_plan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/runtime/film/film_array_file.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_stock_bundle.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

constexpr std::array<FilmFormat, 3> kFormats{FilmFormat::Film16mm, FilmFormat::Film35mm,
                                             FilmFormat::Film70mm};

/// The grain data of one film, with its own storage. The constants are those of the shipped bundle
/// (lab step P15); the curves have no physical meaning.
struct GrainStockValues {
  std::array<double, 3>       cloud_sigma_um{2.0, 2.0, 2.8};
  std::array<double, 3>       layer_correlation{0.489, 0.2349, 0.1743};
  std::array<double, 3>       post_value{0.188, 0.191, 0.172};
  std::array<double, 3>       post_sigma_48{0.0107, 0.0100, 0.0152};
  std::array<double, 3>       post_correlation{0.535, 0.509, 0.087};
  // (3, 4). The third layer counts 3 nodes, and its fourth node repeats the third.
  std::vector<double>         layer_x{0.1, 0.5, 1.0, 2.0, 0.2, 0.6, 1.1, 2.1, 0.3, 0.9, 1.5, 1.5};
  std::vector<double>         layer_sigma{0.010, 0.020, 0.015, 0.012, 0.011, 0.021,
                                  0.016, 0.013, 0.030, 0.040, 0.035, 0.035};
  std::array<std::int32_t, 3> layer_count{4, 4, 3};
  std::vector<double>         channel_x{0.2, 0.8, 1.4, 2.4, 0.3, 0.9, 1.5, 2.5, 1.0, 1.3, 2.0, 3.0};
  std::vector<double>         channel_sigma{0.0056, 0.0108, 0.0084, 0.0048, 0.0052, 0.0108,
                                    0.0082, 0.0062, 0.0118, 0.0158, 0.0110, 0.0042};
  std::array<std::int32_t, 3> channel_count{4, 4, 4};

  [[nodiscard]] auto          View() const -> FilmGrainStockView {
    return {
                 .layer   = {layer_x, layer_sigma, layer_count, 4},
                 .channel = {channel_x, channel_sigma, channel_count, 4},
                 .common  = {.cloud_sigma_um           = cloud_sigma_um,
                             .layer_correlation        = layer_correlation,
                             .post_display_value       = post_value,
                             .post_display_sigma_48    = post_sigma_48,
                             .post_display_correlation = post_correlation,
                             .aperture_area_um2        = std::numbers::pi * 24.0 * 24.0},
                 .tier    = "curve",
    };
  }
};

/// A full frame with a 3:2 aspect ratio and this long side, rendered at @p render_scale.
auto Frame(std::uint32_t long_side, float render_scale = 1.0f) -> ResolvedRenderGeometry {
  const Extent2D    extent{long_side, long_side * 2U / 3U};
  ResolutionRequest resolution;
  resolution.render_scale = render_scale;
  return ResolveRenderGeometry(MakeSourceGeometry(extent, extent), {}, {}, resolution, {});
}

auto Plan(FilmFormat format, std::uint32_t long_side, float render_scale = 1.0f,
          FilmGrainTarget target = FilmGrainTarget::Negative, float strength = 1.0f)
    -> FilmGrainPlan {
  const GrainStockValues stock;
  return MakeFilmGrainPlan(Frame(long_side, render_scale), format, stock.View(), target, strength);
}

/// The full one-axis kernel of a layer.
auto FullKernel(const FilmGrainLayerKernel& kernel) -> std::vector<double> {
  std::vector<double> full;
  for (std::uint32_t index = 0; index <= 2 * kernel.radius; ++index) {
    const auto tap = static_cast<int>(index) - static_cast<int>(kernel.radius);
    full.push_back(kernel.weights[static_cast<std::size_t>(std::abs(tap))]);
  }
  return full;
}

/// The message of the std::invalid_argument of @p make, or empty when it does not throw.
template <class Make>
auto InvalidArgumentText(Make make) -> std::string {
  try {
    (void)make();
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return {};
}

/// Composite Simpson rule of the frequency form of rule 'grain_pixel_integral', two sides.
auto FrequencyFormIntegral(double sigma_c, double pitch, int intervals) -> double {
  const double top       = 4.0 / sigma_c;  // the cloud term is below exp(-631) there
  const double step      = top / intervals;
  const auto   integrand = [&](double f) {
    const double x    = std::numbers::pi * pitch * f;
    const double sinc = f == 0.0 ? 1.0 : std::sin(x) / x;
    return std::exp(-4.0 * std::numbers::pi * std::numbers::pi * sigma_c * sigma_c * f * f) * sinc *
           sinc;
  };
  double sum = integrand(0.0) + integrand(top);
  for (int index = 1; index < intervals; ++index) {
    sum += (index % 2 == 1 ? 4.0 : 2.0) * integrand(step * index);
  }
  return 2.0 * sum * step / 3.0;
}

TEST(FilmGrainPlan, PixelSigmaOfTheGreenLayerMatchesTheTableOfThePlan) {
  // Section 3.2: sigma_48 0.0070 of the 5207 green layer, sigma_c 2.0 um. The values are those of
  // the lab evaluator (grain_reference.json, check plan_table_green_sigma_48_0_0070).
  struct Row {
    FilmFormat    format;
    std::uint32_t long_side;
    double        pitch_um;
    double        sigma_px;
  };
  const std::array<Row, 9> rows{{{FilmFormat::Film16mm, 2560, 4.891, 0.0340},
                                 {FilmFormat::Film16mm, 6000, 2.087, 0.0402},
                                 {FilmFormat::Film16mm, 9504, 1.317, 0.0413},
                                 {FilmFormat::Film35mm, 2560, 14.062, 0.0178},
                                 {FilmFormat::Film35mm, 6000, 6.000, 0.0312},
                                 {FilmFormat::Film35mm, 9504, 3.788, 0.0367},
                                 {FilmFormat::Film70mm, 2560, 27.344, 0.0100},
                                 {FilmFormat::Film70mm, 6000, 11.667, 0.0206},
                                 {FilmFormat::Film70mm, 9504, 7.365, 0.0281}}};
  for (const auto& row : rows) {
    SCOPED_TRACE(std::string(FilmFormatId(row.format)) + " " + std::to_string(row.long_side));
    const auto plan = Plan(row.format, row.long_side);
    EXPECT_NEAR(plan.reference_pitch_um, row.pitch_um, 6e-4);  // 3 decimals in the table
    EXPECT_DOUBLE_EQ(plan.render_pitch_um, plan.reference_pitch_um);
    const double sigma_px = 0.0070 * plan.layers[1].pixel_factor;
    EXPECT_NEAR(sigma_px / row.sigma_px, 1.0, 0.01) << sigma_px;
    EXPECT_DOUBLE_EQ(plan.layers[1].amplitude,
                     plan.layers[1].pixel_factor / plan.layers[1].weight_square_sum);
  }
}

TEST(FilmGrainPlan, SmallerFrameGivesLargerGrainAtTheSameViewSize) {
  // Section 3.2: at 2560 px, 16mm has 1.9 times and 70mm 0.56 times the grain of 35mm.
  const auto r16 = Plan(FilmFormat::Film16mm, 2560).layers[1].pixel_factor /
                   Plan(FilmFormat::Film35mm, 2560).layers[1].pixel_factor;
  const auto r70 = Plan(FilmFormat::Film70mm, 2560).layers[1].pixel_factor /
                   Plan(FilmFormat::Film35mm, 2560).layers[1].pixel_factor;
  EXPECT_NEAR(r16, 1.9, 0.02);
  EXPECT_NEAR(r70, 0.56, 0.01);
}

TEST(FilmGrainPlan, SelwynLawHoldsForALargePixel) {
  // I p goes to 1 for a pixel much larger than the cloud: within 1 % at 500 um. At 50 um the lab
  // evaluator gives 0.95486.
  EXPECT_NEAR(FilmGrainPixelIntegral(2.0, 500.0) * 500.0, 1.0, 0.01);
  EXPECT_NEAR(FilmGrainPixelIntegral(2.0, 500.0) * 500.0, 0.99549, 1e-5);
  EXPECT_NEAR(FilmGrainPixelIntegral(2.0, 50.0) * 50.0, 0.95486, 1e-5);
  EXPECT_NEAR(FilmGrainPixelIntegral(2.0, 250.0) * 250.0, 0.99097, 1e-5);
}

TEST(FilmGrainPlan, PixelIntegralEqualsTheQuadratureOfTheFrequencyForm) {
  // Data rule of G1: a fixed quadrature of at least 2000 points per axis. 20000 Simpson intervals
  // give at least 80 points in each period of sinc^2 for these pitches.
  for (const double sigma_c : {1.0, 2.0, 2.8, 4.0}) {
    for (const double pitch : {1.0, 2.09, 6.0, 14.06, 27.34, 60.0}) {
      SCOPED_TRACE(std::to_string(sigma_c) + " um cloud, " + std::to_string(pitch) + " um pixel");
      const double closed     = FilmGrainPixelIntegral(sigma_c, pitch);
      const double quadrature = FrequencyFormIntegral(sigma_c, pitch, 20000);
      EXPECT_NEAR(closed / quadrature, 1.0, 1e-6);
    }
  }
}

TEST(FilmGrainPlan, KernelWeightsSumToOneAndGiveTheLag1OfThePixelField) {
  for (const auto format : kFormats) {
    for (const std::uint32_t long_side : {2560U, 6000U, 9504U}) {
      const auto plan = Plan(format, long_side);
      for (std::size_t layer = 0; layer < 3; ++layer) {
        SCOPED_TRACE(std::string(FilmFormatId(format)) + " " + std::to_string(long_side) +
                     " layer " + std::to_string(layer));
        const auto& kernel = plan.layers[layer];
        const auto  full   = FullKernel(kernel);
        double      sum    = 0.0;
        double      square = 0.0;
        double      lag    = 0.0;
        for (std::size_t index = 0; index < full.size(); ++index) {
          sum += full[index];
          square += full[index] * full[index];
          lag += index + 1 < full.size() ? full[index] * full[index + 1] : 0.0;
        }
        EXPECT_NEAR(sum, 1.0, 1e-12);
        EXPECT_NEAR(square, kernel.weight_square_sum, 1e-12);
        EXPECT_NEAR(lag / square, kernel.lag1, 1e-12);
        EXPECT_EQ(kernel.radius, static_cast<std::uint32_t>(std::ceil(3.0 * kernel.sigma_b_px)));
        EXPECT_LE(kernel.radius, kFilmGrainMaxBlurRadius);
        for (std::uint32_t tap = kernel.radius + 1; tap <= kFilmGrainMaxBlurRadius; ++tap) {
          EXPECT_EQ(kernel.weights[tap], 0.0) << tap;
        }
        // Rule 'grain_kernel': the bisection matches the lag-1 of the continuous field.
        EXPECT_NEAR(kernel.lag1, FilmGrainPixelLag1(kernel.cloud_sigma_um, plan.render_pitch_um),
                    1e-9);
      }
    }
  }
}

TEST(FilmGrainPlan, AggregateFollowsTheRenderScale) {
  const std::array<float, 4>         scales{1.0f, 0.5f, 0.3f, 0.1f};
  const std::array<std::uint32_t, 4> aggregates{1, 2, 3, 4};
  const auto                         full = Plan(FilmFormat::Film35mm, 6000);
  for (std::size_t index = 0; index < scales.size(); ++index) {
    SCOPED_TRACE(scales[index]);
    const auto plan = Plan(FilmFormat::Film35mm, 6000, scales[index]);
    EXPECT_EQ(plan.aggregate, aggregates[index]);
    EXPECT_NEAR(plan.render_scale, scales[index], 1e-6);
    EXPECT_DOUBLE_EQ(plan.reference_pitch_um, full.reference_pitch_um);
    EXPECT_NEAR(plan.render_pitch_um, full.reference_pitch_um / scales[index], 1e-4);
  }
}

TEST(FilmGrainPlan, HalfScaleRenderHasTheKernelOfTwiceThePitch) {
  const auto half = Plan(FilmFormat::Film16mm, 6000, 0.5f);
  const auto direct =
      MakeFilmGrainLayerKernel(2.0, std::numbers::pi * 24.0 * 24.0, 2.0 * 12.52 * 1000.0 / 6000.0);
  EXPECT_NEAR(half.layers[0].pixel_factor, direct.pixel_factor, 1e-9);
  EXPECT_NEAR(half.layers[0].sigma_b_px, direct.sigma_b_px, 1e-9);
}

TEST(FilmGrainPlan, TargetSelectsTheCurvesAndTheCorrelation) {
  const auto negative = Plan(FilmFormat::Film35mm, 2560, 1.0f, FilmGrainTarget::Negative);
  const auto post     = Plan(FilmFormat::Film35mm, 2560, 1.0f, FilmGrainTarget::Post);
  EXPECT_EQ(negative.target, FilmGrainTarget::Negative);
  EXPECT_FLOAT_EQ(negative.curves[0].first_sigma, 0.010f);
  EXPECT_FLOAT_EQ(post.curves[0].first_sigma, 0.0056f);
  // Row 1 of L is (rg, sqrt(1 - rg^2), 0).
  EXPECT_DOUBLE_EQ(negative.mix[3], 0.489);
  EXPECT_DOUBLE_EQ(post.mix[3], 0.535);
  for (const auto* plan : {&negative, &post}) {
    const auto& l = plan->mix;
    EXPECT_EQ(l[1], 0.0);
    EXPECT_EQ(l[2], 0.0);
    EXPECT_EQ(l[5], 0.0);
    // L L^T has a unit diagonal, so each mixed field keeps unit variance.
    for (std::size_t row = 0; row < 3; ++row) {
      EXPECT_NEAR(l[row * 3] * l[row * 3] + l[row * 3 + 1] * l[row * 3 + 1] +
                      l[row * 3 + 2] * l[row * 3 + 2],
                  1.0, 1e-12);
    }
  }
  // The off-diagonal of L L^T is the correlation: gb is row 2 against row 1.
  const auto& l = negative.mix;
  EXPECT_NEAR(l[6] * l[3] + l[7] * l[4], 0.2349, 1e-12);
  EXPECT_NEAR(l[6], 0.1743, 1e-12);
  EXPECT_EQ(post.layers[1].pixel_factor, negative.layers[1].pixel_factor);
}

TEST(FilmGrainPlan, StrengthZeroDisablesThePlanAndKeepsItsValues) {
  const auto off = Plan(FilmFormat::Film35mm, 6000, 1.0f, FilmGrainTarget::Post, 0.0f);
  const auto on  = Plan(FilmFormat::Film35mm, 6000, 1.0f, FilmGrainTarget::Post, 1.4f);
  EXPECT_FALSE(off.enabled);
  EXPECT_TRUE(on.enabled);
  EXPECT_DOUBLE_EQ(on.strength, static_cast<double>(1.4f));
  EXPECT_EQ(off.layers[2].amplitude, on.layers[2].amplitude);
  EXPECT_TRUE(Plan(FilmFormat::Film35mm, 6000, 1.0f, FilmGrainTarget::Post, 2.0f).enabled);
}

TEST(FilmGrainPlan, CurveSegmentsReproduceTheNodesAndHoldTheEndValues) {
  const GrainStockValues stock;
  const auto             view = stock.View();
  for (std::size_t layer = 0; layer < 3; ++layer) {
    SCOPED_TRACE(layer);
    const auto curve = MakeFilmGrainCurve(view.layer, layer, "layer");
    const auto count = static_cast<std::size_t>(stock.layer_count[layer]);
    const auto x     = std::span(stock.layer_x).subspan(layer * 4, count);
    const auto sigma = std::span(stock.layer_sigma).subspan(layer * 4, count);
    for (std::size_t node = 0; node < count; ++node) {
      EXPECT_NEAR(FilmGrainCurveSigma(curve, static_cast<float>(x[node])), sigma[node], 1e-7);
    }
    for (std::size_t node = 0; node + 1 < count; ++node) {
      const double middle   = 0.5 * (x[node] + x[node + 1]);
      const double expected = 0.5 * (sigma[node] + sigma[node + 1]);
      EXPECT_NEAR(FilmGrainCurveSigma(curve, static_cast<float>(middle)), expected, 1e-7);
    }
    EXPECT_NEAR(FilmGrainCurveSigma(curve, static_cast<float>(x.front() - 1.0)), sigma.front(),
                1e-7);
    EXPECT_NEAR(FilmGrainCurveSigma(curve, static_cast<float>(x.back() + 5.0)), sigma.back(), 1e-7);
    for (std::size_t segment = count - 1; segment < FILM_GRAIN_CURVE_SEGMENTS; ++segment) {
      EXPECT_EQ(curve.delta[segment], 0.0f);
      EXPECT_EQ(curve.inverse_width[segment], 0.0f);
    }
  }
}

TEST(FilmGrainPlan, InvalidInputThrowsWithTheNameOfTheField) {
  GrainStockValues stock;
  const auto       frame = Frame(6000);
  const auto       make  = [&](FilmFormat format, float strength, FilmGrainTarget target) {
    return [&, format, strength, target] {
      return MakeFilmGrainPlan(frame, format, stock.View(), target, strength);
    };
  };
  const auto negative = FilmGrainTarget::Negative;
  EXPECT_NE(
      InvalidArgumentText(make(static_cast<FilmFormat>(7), 1.0f, negative)).find("film_format"),
      std::string::npos);
  for (const float strength : {-0.1f, 2.5f, std::nanf("")}) {
    SCOPED_TRACE(strength);
    EXPECT_NE(InvalidArgumentText(make(FilmFormat::Film35mm, strength, negative)).find("strength"),
              std::string::npos);
  }
  EXPECT_NE(InvalidArgumentText([&] {
              return MakeFilmGrainPlan(ResolvedRenderGeometry{}, FilmFormat::Film35mm, stock.View(),
                                       negative, 1.0f);
            }).find("full_reference_extent"),
            std::string::npos);

  stock.layer_count[1] = 1;
  EXPECT_NE(InvalidArgumentText(make(FilmFormat::Film35mm, 1.0f, negative)).find("layer curve"),
            std::string::npos);
  // The post path reads the channel curves only.
  EXPECT_NO_THROW((void)make(FilmFormat::Film35mm, 1.0f, FilmGrainTarget::Post)());
  stock.layer_count[1] = 4;
  stock.layer_x[5]     = stock.layer_x[4];
  EXPECT_NE(InvalidArgumentText(make(FilmFormat::Film35mm, 1.0f, negative)).find("does not rise"),
            std::string::npos);
  stock.layer_x[5]        = 0.6;

  stock.cloud_sigma_um[2] = 0.0;
  EXPECT_NE(
      InvalidArgumentText(make(FilmFormat::Film35mm, 1.0f, negative)).find("grain_cloud_sigma_um"),
      std::string::npos);
  stock.cloud_sigma_um[2] = 2.8;

  // rg 0.9, rb 0.9, gb -0.9 has no real Cholesky factor.
  stock.layer_correlation = {0.9, -0.9, 0.9};
  EXPECT_NE(InvalidArgumentText(make(FilmFormat::Film35mm, 1.0f, negative))
                .find("grain_layer_correlation"),
            std::string::npos);
}

TEST(FilmFormatTest, IdentifiersAndFrameSizesFollowDecisionU1) {
  EXPECT_EQ(FilmFormatId(FilmFormat::Film16mm), "16mm");
  EXPECT_EQ(FilmFormatId(FilmFormat::Film35mm), "35mm");
  EXPECT_EQ(FilmFormatId(FilmFormat::Film70mm), "70mm");
  EXPECT_DOUBLE_EQ(FilmFrameLongSideMm(FilmFormat::Film16mm), 12.52);
  EXPECT_DOUBLE_EQ(FilmFrameLongSideMm(FilmFormat::Film35mm), 36.0);
  EXPECT_DOUBLE_EQ(FilmFrameLongSideMm(FilmFormat::Film70mm), 70.0);
  for (const auto format : kFormats) {
    EXPECT_EQ(TryParseFilmFormat(FilmFormatId(format)), format);
  }
  EXPECT_FALSE(TryParseFilmFormat("super35").has_value());
  EXPECT_FALSE(TryParseFilmFormat("").has_value());
  EXPECT_THROW((void)FilmFrameLongSideMm(static_cast<FilmFormat>(3)), std::invalid_argument);
}

#if defined(ALCEDO_FILM_PRINT_EXPECTED_DIR)

/// The values of `grain__expected.bin` of the lab evaluator (grain_reference.expected_arrays).
class FilmGrainPlanLabTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    const auto path = fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "grain__expected.bin";
    expected_ =
        std::make_unique<FilmArrayFile>(FilmArrayFile::Parse(ReadFilmResourceBytes(path), path));
  }
  static void                                  TearDownTestSuite() { expected_.reset(); }

  static inline std::unique_ptr<FilmArrayFile> expected_;
};

TEST_F(FilmGrainPlanLabTest, KernelAndPixelFactorMatchTheLabEvaluator) {
  const auto&            file        = *expected_;
  const auto             long_sides  = file.I32("long_sides", {2});
  const auto             pitch       = file.F64("pitch_um", {3, 2});
  const auto             sigma_b     = file.F64("sigma_b_px", {3, 2, 3});
  const auto             radius      = file.I32("blur_radius", {3, 2, 3});
  const auto             square_sum  = file.F64("weight_square_sum", {3, 2, 3});
  const auto             factor      = file.F64("pixel_factor", {3, 2, 3});
  const auto             lag1        = file.F64("lag1_kernel", {3, 2, 3});
  const auto             lag1_field  = file.F64("lag1_continuous", {3, 2, 3});
  const auto             half_factor = file.F64("zoom_half_render_factor", {3, 2, 3});
  const auto             cloud       = file.F64("cloud_sigma_um", {3});
  const GrainStockValues stock;
  for (std::size_t layer = 0; layer < 3; ++layer) {
    ASSERT_EQ(cloud[layer], stock.cloud_sigma_um[layer]);
  }
  for (std::size_t f = 0; f < 3; ++f) {
    for (std::size_t s = 0; s < 2; ++s) {
      const auto side = static_cast<std::uint32_t>(long_sides[s]);
      const auto plan = Plan(kFormats[f], side);
      const auto half = Plan(kFormats[f], side, 0.5f);
      EXPECT_NEAR(plan.reference_pitch_um, pitch[f * 2 + s], 1e-12);
      for (std::size_t layer = 0; layer < 3; ++layer) {
        SCOPED_TRACE(std::string(FilmFormatId(kFormats[f])) + " " + std::to_string(side) +
                     " layer " + std::to_string(layer));
        const auto  index  = (f * 2 + s) * 3 + layer;
        const auto& kernel = plan.layers[layer];
        EXPECT_NEAR(kernel.sigma_b_px, sigma_b[index], 1e-6);
        EXPECT_EQ(kernel.radius, static_cast<std::uint32_t>(radius[index]));
        EXPECT_NEAR(kernel.weight_square_sum, square_sum[index], 1e-6);
        EXPECT_NEAR(kernel.pixel_factor, factor[index], 1e-6);
        EXPECT_NEAR(kernel.lag1, lag1[index], 1e-6);
        EXPECT_NEAR(kernel.lag1, lag1_field[index], 1e-6);
        EXPECT_NEAR(half.layers[layer].pixel_factor, half_factor[index], 1e-6);
      }
    }
  }
}

TEST_F(FilmGrainPlanLabTest, AggregateAndMixMatchTheLabEvaluator) {
  const auto& file       = *expected_;
  const auto  scales     = file.F64("render_scales", {4});
  const auto  aggregates = file.I32("aggregate", {4});
  for (std::size_t index = 0; index < 4; ++index) {
    const auto plan = Plan(FilmFormat::Film35mm, 6000, static_cast<float>(scales[index]));
    EXPECT_EQ(plan.aggregate, static_cast<std::uint32_t>(aggregates[index])) << scales[index];
  }
  GrainStockValues stock;
  const auto       layer = file.F64("layer_correlation", {3});
  const auto       post  = file.F64("post_display_correlation", {3});
  std::copy(layer.begin(), layer.end(), stock.layer_correlation.begin());
  std::copy(post.begin(), post.end(), stock.post_correlation.begin());
  const auto negative_plan = MakeFilmGrainPlan(Frame(6000), FilmFormat::Film35mm, stock.View(),
                                               FilmGrainTarget::Negative, 1.0f);
  const auto post_plan     = MakeFilmGrainPlan(Frame(6000), FilmFormat::Film35mm, stock.View(),
                                               FilmGrainTarget::Post, 1.0f);
  const auto layer_l       = file.F64("layer_cholesky", {3, 3});
  const auto post_l        = file.F64("post_cholesky", {3, 3});
  for (std::size_t entry = 0; entry < 9; ++entry) {
    EXPECT_NEAR(negative_plan.mix[entry], layer_l[entry], 1e-12) << entry;
    EXPECT_NEAR(post_plan.mix[entry], post_l[entry], 1e-12) << entry;
  }
}

#if defined(ALCEDO_FILM_PRINT_RESOURCE_DIR)

TEST_F(FilmGrainPlanLabTest, PlanOfEachPrintStockGivesTheLabPixelSigmaAtGrey) {
  const auto  bundle = FilmStockBundle::Open(fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR));
  const auto& file   = *expected_;
  const auto  stocks = file.Metadata().at("print_stocks").get<std::vector<std::string>>();
  ASSERT_EQ(stocks.size(), 4u);
  const auto sides    = file.I32("long_sides", {2});
  const auto amount   = file.F64("print_amount", {4, 3});
  const auto sigma_48 = file.F64("print_sigma_amount_48", {4, 3});
  const auto pixel    = file.F64("print_pixel_sigma", {4, 3, 2, 3});
  for (std::size_t stock = 0; stock < stocks.size(); ++stock) {
    const auto view = bundle->Grain(stocks[stock]);
    for (std::size_t f = 0; f < 3; ++f) {
      for (std::size_t s = 0; s < 2; ++s) {
        const auto plan = MakeFilmGrainPlan(Frame(static_cast<std::uint32_t>(sides[s])),
                                            kFormats[f], view, FilmGrainTarget::Negative, 1.0f);
        for (std::size_t layer = 0; layer < 3; ++layer) {
          SCOPED_TRACE(stocks[stock] + " " + std::string(FilmFormatId(kFormats[f])) + " " +
                       std::to_string(sides[s]) + " layer " + std::to_string(layer));
          const double curve = FilmGrainCurveSigma(plan.curves[layer],
                                                   static_cast<float>(amount[stock * 3 + layer]));
          EXPECT_NEAR(curve / sigma_48[stock * 3 + layer], 1.0, 1e-5);
          const double expected = pixel[((stock * 3 + f) * 2 + s) * 3 + layer];
          EXPECT_NEAR(curve * plan.layers[layer].pixel_factor / expected, 1.0, 1e-5);
        }
      }
    }
  }
  const auto post = bundle->GrainConstants().post_display_sigma_48;
  const auto lab  = file.F64("post_display_sigma_48_linear", {3});
  for (std::size_t channel = 0; channel < 3; ++channel) {
    EXPECT_DOUBLE_EQ(post[channel], lab[channel]);
  }
}

#endif  // ALCEDO_FILM_PRINT_RESOURCE_DIR

#else

TEST(FilmGrainPlanLabTest, LabValuesAreConfigured) {
  GTEST_SKIP() << "ALCEDO_FILM_PRINT_EXPECTED_DIR is not configured, so the comparison with the "
                  "lab evaluator did not run.";
}

#endif  // ALCEDO_FILM_PRINT_EXPECTED_DIR

// FilmGrainField: the hash and the Gaussian of film_grain_field.h on the host.

constexpr unsigned int kSeed = 0x6a09e667u;

TEST(FilmGrainField, GaussianOfAMillionHashesHasUnitMomentsAndNormalKurtosis) {
  constexpr int         kSide = 1000;
  std::array<double, 3> sum{};
  std::array<double, 3> square{};
  std::array<double, 3> fourth{};
  for (int y = 0; y < kSide; ++y) {
    for (int x = 0; x < kSide; ++x) {
      const auto bits =
          FilmGrainHash3(static_cast<unsigned int>(x), static_cast<unsigned int>(y), kSeed);
      const std::array<double, 3> g{FilmGrainGaussian(bits.x), FilmGrainGaussian(bits.y),
                                    FilmGrainGaussian(bits.z)};
      for (std::size_t c = 0; c < 3; ++c) {
        sum[c] += g[c];
        square[c] += g[c] * g[c];
        fourth[c] += g[c] * g[c] * g[c] * g[c];
      }
    }
  }
  const double n = static_cast<double>(kSide) * kSide;
  for (std::size_t c = 0; c < 3; ++c) {
    SCOPED_TRACE(c);
    const double mean     = sum[c] / n;
    const double variance = square[c] / n - mean * mean;
    EXPECT_NEAR(mean, 0.0, 0.005);
    EXPECT_NEAR(std::sqrt(variance), 1.0, 0.005);
    EXPECT_NEAR(fourth[c] / n / (variance * variance), 3.0, 0.05);
  }
}

TEST(FilmGrainField, NeighbourHashesAndChannelsAreUncorrelated) {
  constexpr int                     kSide = 1001;
  std::vector<std::array<float, 3>> field(static_cast<std::size_t>(kSide) * kSide);
  for (int y = 0; y < kSide; ++y) {
    for (int x = 0; x < kSide; ++x) {
      const auto bits =
          FilmGrainHash3(static_cast<unsigned int>(x), static_cast<unsigned int>(y), kSeed);
      field[static_cast<std::size_t>(y) * kSide + x] = {
          FilmGrainGaussian(bits.x), FilmGrainGaussian(bits.y), FilmGrainGaussian(bits.z)};
    }
  }
  // The sample mean is 0 within 0.005, so a plain mean of products is the correlation.
  const auto correlation = [&](int dx, int dy, std::size_t a, std::size_t b) {
    double product = 0.0;
    double sa      = 0.0;
    double sb      = 0.0;
    for (int y = 0; y + dy < kSide; ++y) {
      for (int x = 0; x + dx < kSide; ++x) {
        const double u = field[static_cast<std::size_t>(y) * kSide + x][a];
        const double v = field[static_cast<std::size_t>(y + dy) * kSide + x + dx][b];
        product += u * v;
        sa += u * u;
        sb += v * v;
      }
    }
    return product / std::sqrt(sa * sb);
  };
  for (std::size_t c = 0; c < 3; ++c) {
    SCOPED_TRACE(c);
    EXPECT_LT(std::abs(correlation(1, 0, c, c)), 0.005);
    EXPECT_LT(std::abs(correlation(0, 1, c, c)), 0.005);
    EXPECT_LT(std::abs(correlation(1, 1, c, c)), 0.005);
  }
  EXPECT_LT(std::abs(correlation(0, 0, 0, 1)), 0.005);
  EXPECT_LT(std::abs(correlation(0, 0, 1, 2)), 0.005);
  EXPECT_LT(std::abs(correlation(0, 0, 0, 2)), 0.005);
}

TEST(FilmGrainField, GaussianIsTheInverseNormalDistribution) {
  // The exact inverse of the normal distribution by Newton steps on erfc, in double precision.
  const auto inverse_normal = [](double u) {
    double z = 0.0;
    for (int step = 0; step < 100; ++step) {
      const double cdf     = 0.5 * std::erfc(-z / std::numbers::sqrt2);
      const double density = std::exp(-0.5 * z * z) / std::sqrt(2.0 * std::numbers::pi);
      z -= (cdf - u) / density;
    }
    return z;
  };
  for (const unsigned int high : {0u, 1u, 100u, 40000u, 1u << 20, 3u << 21, (1u << 23) - 1u}) {
    const unsigned int bits = high << 9u;
    const double       u    = (static_cast<double>(high) + 0.5) / 8388608.0;
    const double       z    = inverse_normal(u);
    SCOPED_TRACE(u);
    EXPECT_NEAR(FilmGrainGaussian(bits), z, 1e-5 * (std::max)(1.0, std::abs(z)));
  }
  // The low 9 bits do not change the value; the 23 high bits give a symmetric result.
  EXPECT_EQ(FilmGrainGaussian(0x12345600u), FilmGrainGaussian(0x123457ffu));
  EXPECT_NEAR(FilmGrainGaussian(0u), -FilmGrainGaussian(0xffffffffu), 1e-6);
  EXPECT_LT(FilmGrainGaussian(0u), -5.0f);
}

TEST(FilmGrainField, HashIsAFunctionOfTheCoordinatesAndTheSeed) {
  const auto a = FilmGrainHash3(12u, 34u, kSeed);
  const auto b = FilmGrainHash3(12u, 34u, kSeed);
  const auto c = FilmGrainHash3(12u, 34u, kSeed + 1u);
  const auto d = FilmGrainHash3(34u, 12u, kSeed);
  EXPECT_EQ(a.x, b.x);
  EXPECT_EQ(a.y, b.y);
  EXPECT_EQ(a.z, b.z);
  EXPECT_NE(a.x, c.x);
  EXPECT_NE(a.x, d.x);
}

}  // namespace
}  // namespace alcedo
