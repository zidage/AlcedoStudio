//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Colour statistics of a film grain residual (output - input), for the film grain tests of the host
// math and of each GPU backend.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace alcedo::film_grain_test {

struct GrainResidualColour {
  std::array<double, 3> std_dev{};  ///< r, g, b
  double                correlation_rg               = 0.0;
  double                correlation_gb               = 0.0;
  double                correlation_rb               = 0.0;
  /// Shares of the residual variance on the neutral (1, 1, 1), warm-cool (1, 0, -1), and
  /// green-magenta (-1, 2, -1) axes. They add up to 1.
  double                neutral_share                = 0.0;
  double                warm_cool_share              = 0.0;
  double                green_magenta_share          = 0.0;
  /// Correlation of the luminance residual (Rec. 709 weights) with the warm-cool residual
  /// r - (g + b) / 2. Positive: grain that brightens is warm and grain that darkens is cool, as in
  /// real colour negative film. Near a neutral grey the encoded and the linear residual give the
  /// same value.
  double                lightness_warmth_correlation = 0.0;
};

/// @p input and @p output hold pixels with members r, g, b, in the same order.
template <typename Pixel>
auto MeasureGrainResidualColour(const std::vector<Pixel>& input, const std::vector<Pixel>& output)
    -> GrainResidualColour {
  const std::size_t     count = input.size() < output.size() ? input.size() : output.size();
  std::array<double, 3> mean{};
  for (std::size_t i = 0; i < count; ++i) {
    mean[0] += static_cast<double>(output[i].r) - static_cast<double>(input[i].r);
    mean[1] += static_cast<double>(output[i].g) - static_cast<double>(input[i].g);
    mean[2] += static_cast<double>(output[i].b) - static_cast<double>(input[i].b);
  }
  for (double& value : mean) value /= static_cast<double>(count);
  double c[3][3] = {};
  for (std::size_t i = 0; i < count; ++i) {
    const double d[3] = {
        static_cast<double>(output[i].r) - static_cast<double>(input[i].r) - mean[0],
        static_cast<double>(output[i].g) - static_cast<double>(input[i].g) - mean[1],
        static_cast<double>(output[i].b) - static_cast<double>(input[i].b) - mean[2]};
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) c[a][b] += d[a] * d[b];
    }
  }
  for (auto& row : c) {
    for (double& value : row) value /= static_cast<double>(count);
  }
  GrainResidualColour result;
  for (int a = 0; a < 3; ++a) result.std_dev[a] = std::sqrt(c[a][a]);
  result.correlation_rg = c[0][1] / (result.std_dev[0] * result.std_dev[1]);
  result.correlation_gb = c[1][2] / (result.std_dev[1] * result.std_dev[2]);
  result.correlation_rb = c[0][2] / (result.std_dev[0] * result.std_dev[2]);
  const auto share      = [&c](double x, double y, double z) {
    const double v[3] = {x, y, z};
    double       sum  = 0.0;
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) sum += v[a] * c[a][b] * v[b];
    }
    return sum / (x * x + y * y + z * z);
  };
  const double trace         = c[0][0] + c[1][1] + c[2][2];
  result.neutral_share       = share(1.0, 1.0, 1.0) / trace;
  result.warm_cool_share     = share(1.0, 0.0, -1.0) / trace;
  result.green_magenta_share = share(-1.0, 2.0, -1.0) / trace;
  const double luminance[3]  = {0.2126, 0.7152, 0.0722};
  const double warmth[3]     = {1.0, -0.5, -0.5};
  double       cov_lw = 0.0, var_l = 0.0, var_w = 0.0;
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      cov_lw += luminance[a] * c[a][b] * warmth[b];
      var_l += luminance[a] * c[a][b] * luminance[b];
      var_w += warmth[a] * c[a][b] * warmth[b];
    }
  }
  result.lightness_warmth_correlation = cov_lw / std::sqrt(var_l * var_w);
  return result;
}

}  // namespace alcedo::film_grain_test
