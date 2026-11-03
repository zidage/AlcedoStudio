//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_tables.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace alcedo::film_tables {
namespace {

constexpr double  kLn10         = 2.302585092994046;
constexpr int64_t kNodesPerTask = 256;

// body(first, last) runs on chunks of the range, on the worker threads.
template <class Body>
void ParallelRanges(int64_t count, int threads, const Body& body) {
  if (threads <= 0) threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  if (threads == 1 || count <= kNodesPerTask) {
    body(int64_t{0}, count);
    return;
  }
  std::atomic<int64_t> next{0};
  const auto           work = [&] {
    for (;;) {
      const int64_t first = next.fetch_add(kNodesPerTask);
      if (first >= count) return;
      body(first, std::min(first + kNodesPerTask, count));
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  for (int k = 1; k < threads; ++k) pool.emplace_back(work);
  work();
  for (auto& thread : pool) thread.join();
}

inline void Transmission(const SpectralBand& band, const double* a, double* t) {
  const int64_t n  = band.wavelengths;
  const double* d0 = band.dyes;
  const double* d1 = band.dyes + n;
  const double* d2 = band.dyes + 2 * n;
  for (int64_t w = 0; w < n; ++w)
    t[w] = std::exp(-kLn10 * (band.base[w] + a[0] * d0[w] + a[1] * d1[w] + a[2] * d2[w]));
}

inline void BandSums(const SpectralBand& band, const double* t, double total[kLayers]) {
  const int64_t n = band.wavelengths;
  for (int k = 0; k < kLayers; ++k) {
    const double* weight = band.weights + k * n;
    double        sum    = 0.0;
    for (int64_t w = 0; w < n; ++w) sum += t[w] * weight[w];
    total[k] = sum;
  }
}

void WeightTotals(const SpectralBand& band, double total[kLayers]) {
  for (int k = 0; k < kLayers; ++k) {
    total[k] = 0.0;
    for (int64_t w = 0; w < band.wavelengths; ++w)
      total[k] += band.weights[k * band.wavelengths + w];
  }
}

// One node, with the steps and the stop rule of fast_kernels._solve. Returns the largest residual.
double SolveNode(const SpectralBand& s, const double target[kLayers], const double total[kLayers],
                 int iterations, double a[kLayers], double* t) {
  const int64_t n  = s.wavelengths;
  const double* d0 = s.dyes;
  const double* d1 = s.dyes + n;
  const double* d2 = s.dyes + 2 * n;
  for (int k = 0; k < kLayers; ++k) a[k] = std::max(target[k], 0.0);
  double residual = 0.0;
  for (int iteration = 0; iteration <= iterations; ++iteration) {
    Transmission(s, a, t);
    double jacobian[kLayers][kLayers];
    double r[kLayers];
    for (int i = 0; i < kLayers; ++i) {
      const double* weight = s.weights + i * n;
      double        sum = 0.0, j0 = 0.0, j1 = 0.0, j2 = 0.0;
      for (int64_t w = 0; w < n; ++w) {
        const double x = t[w] * weight[w];
        sum += x;
        j0 += x * d0[w];
        j1 += x * d1[w];
        j2 += x * d2[w];
      }
      r[i]           = -std::log10(sum / total[i]) - target[i];
      jacobian[i][0] = j0 / sum;
      jacobian[i][1] = j1 / sum;
      jacobian[i][2] = j2 / sum;
    }
    residual = std::max({std::fabs(r[0]), std::fabs(r[1]), std::fabs(r[2])});
    if (iteration == iterations || residual < 1e-11) break;
    const double j00 = jacobian[0][0] + 1e-9, j01 = jacobian[0][1], j02 = jacobian[0][2];
    const double j10 = jacobian[1][0], j11 = jacobian[1][1] + 1e-9, j12 = jacobian[1][2];
    const double j20 = jacobian[2][0], j21 = jacobian[2][1], j22 = jacobian[2][2] + 1e-9;
    const double c00 = j11 * j22 - j12 * j21, c01 = j12 * j20 - j10 * j22,
                 c02 = j10 * j21 - j11 * j20;
    const double det = j00 * c00 + j01 * c01 + j02 * c02;
    const double s0 =
        (r[0] * c00 + r[1] * (j02 * j21 - j01 * j22) + r[2] * (j01 * j12 - j02 * j11)) / det;
    const double s1 =
        (r[0] * c01 + r[1] * (j00 * j22 - j02 * j20) + r[2] * (j02 * j10 - j00 * j12)) / det;
    const double s2 =
        (r[0] * c02 + r[1] * (j01 * j20 - j00 * j21) + r[2] * (j00 * j11 - j01 * j10)) / det;
    const double n0 = std::max(a[0] - s0, 0.0), n1 = std::max(a[1] - s1, 0.0),
                 n2     = std::max(a[2] - s2, 0.0);
    const double change = std::fabs(n0 - a[0]) + std::fabs(n1 - a[1]) + std::fabs(n2 - a[2]);
    a[0]                = n0;
    a[1]                = n1;
    a[2]                = n2;
    if (change < 1e-12) break;
  }
  return residual;
}

// Printer lights, print contrast, flash, and the paper curve of one layer. outside: 1 when the
// exposure is off the curve.
inline double PaperStatus(const PrintControls& controls, const PaperCurves& curves,
                          double paper_log, int layer, int64_t& outside) {
  const double aim   = controls.aim_log_h[layer];
  double       log_h = paper_log + controls.balance_log10[layer];
  log_h              = aim + controls.contrast[layer] * (log_h - aim);
  if (controls.flash != 0.0)
    log_h = std::log10(std::pow(10.0, log_h) + controls.flash * std::pow(10.0, aim));
  const double  c    = (log_h - curves.log_h_start[layer]) / curves.log_h_step;
  const int64_t last = curves.lengths[layer] - 1;
  if (c < 0.0 || c > static_cast<double>(last)) ++outside;
  const double* curve = curves.density + layer * curves.stored_length;
  if (c <= 0.0) return curve[0];
  if (c >= static_cast<double>(last)) return curve[last];
  const int64_t index = static_cast<int64_t>(c);
  return curve[index] + (c - static_cast<double>(index)) * (curve[index + 1] - curve[index]);
}

}  // namespace

void Log10BandResponse(const SpectralBand& band, const double* amounts, int64_t count, double* out,
                       int threads) {
  ParallelRanges(count, threads, [&](int64_t first, int64_t last) {
    std::vector<double> t(band.wavelengths);
    for (int64_t node = first; node < last; ++node) {
      double total[kLayers];
      Transmission(band, amounts + kLayers * node, t.data());
      BandSums(band, t.data(), total);
      for (int k = 0; k < kLayers; ++k) out[kLayers * node + k] = std::log10(total[k]);
    }
  });
}

int64_t SolveDyeAmounts(const SpectralBand& status, const double* targets, int64_t count,
                        int iterations, double reached_limit, double* amounts, int threads) {
  double total[kLayers];
  WeightTotals(status, total);
  std::atomic<int64_t> not_reached{0};
  ParallelRanges(count, threads, [&](int64_t first, int64_t last) {
    std::vector<double> t(status.wavelengths);
    int64_t             missed = 0;
    for (int64_t node = first; node < last; ++node) {
      if (SolveNode(status, targets + kLayers * node, total, iterations, amounts + kLayers * node,
                    t.data()) > reached_limit)
        ++missed;
    }
    not_reached += missed;
  });
  return not_reached.load();
}

void TableBandIntegral(const float* table, int64_t count, int64_t wavelengths,
                       const double* weights, double* out, int threads) {
  ParallelRanges(count, threads, [&](int64_t first, int64_t last) {
    for (int64_t node = first; node < last; ++node) {
      const float* row = table + node * wavelengths;
      for (int k = 0; k < kLayers; ++k) {
        const double* weight = weights + k * wavelengths;
        double        sum    = 0.0;
        for (int64_t w = 0; w < wavelengths; ++w) sum += static_cast<double>(row[w]) * weight[w];
        out[kLayers * node + k] = sum;
      }
    }
  });
}

void BuildPaperTable(const SpectralBand& status_a, const SpectralBand& viewing, double xyz_scale,
                     const StatusAxes& axes, int iterations, double reached_limit,
                     double* cube_root_xyz, uint8_t* not_reached, int threads) {
  double total[kLayers];
  WeightTotals(status_a, total);
  const int64_t n = axes.nodes;
  ParallelRanges(n * n * n, threads, [&](int64_t first, int64_t last) {
    std::vector<double> t_status(status_a.wavelengths), t_view(viewing.wavelengths);
    for (int64_t node = first; node < last; ++node) {
      const int64_t index[kLayers] = {node / (n * n), node / n % n, node % n};
      double        target[kLayers], amounts[kLayers], xyz[kLayers];
      for (int k = 0; k < kLayers; ++k)
        target[k] = axes.start[k] + axes.range[k] * std::pow(static_cast<double>(index[k]) /
                                                                 static_cast<double>(n - 1),
                                                             axes.power);
      not_reached[node] =
          SolveNode(status_a, target, total, iterations, amounts, t_status.data()) > reached_limit
              ? 1
              : 0;
      Transmission(viewing, amounts, t_view.data());
      BandSums(viewing, t_view.data(), xyz);
      for (int k = 0; k < kLayers; ++k)
        cube_root_xyz[kLayers * node + k] = std::cbrt(xyz_scale * xyz[k]);
    }
  });
}

ComposeCounts ComposePrintTable(const double* paper_log, int64_t count,
                                const PrintControls& controls, const PaperCurves& curves,
                                const StatusAxes& axes, const double* paper_table,
                                const uint8_t* not_reached, double* cube_root_xyz, int threads) {
  const int64_t        n   = axes.nodes;
  const double         top = static_cast<double>(n) - 1.000001;
  std::atomic<int64_t> outside_total{0}, not_reached_total{0};
  ParallelRanges(count, threads, [&](int64_t first, int64_t last) {
    int64_t outside = 0, missed = 0;
    for (int64_t node = first; node < last; ++node) {
      int64_t index[kLayers];
      double  fraction[kLayers];
      int64_t nearest = 0;
      for (int k = 0; k < kLayers; ++k) {
        const double status =
            PaperStatus(controls, curves, paper_log[kLayers * node + k], k, outside);
        const double share = std::max((status - axes.start[k]) / axes.range[k], 0.0);
        const double c =
            std::min(std::pow(share, 1.0 / axes.power) * static_cast<double>(n - 1), top);
        index[k]    = static_cast<int64_t>(c);
        fraction[k] = c - static_cast<double>(index[k]);
        nearest     = nearest * n + index[k] + (fraction[k] >= 0.5 ? 1 : 0);
      }
      missed += not_reached[nearest];
      const int64_t base          = ((index[0] * n + index[1]) * n + index[2]) * kLayers;
      const int64_t step[kLayers] = {n * n * kLayers, n * kLayers, kLayers};
      for (int k = 0; k < kLayers; ++k) {
        double value = 0.0;
        for (int corner = 0; corner < 8; ++corner) {
          const int    x = corner >> 2 & 1, y = corner >> 1 & 1, z = corner & 1;
          const double weight = (x ? fraction[0] : 1.0 - fraction[0]) *
                                (y ? fraction[1] : 1.0 - fraction[1]) *
                                (z ? fraction[2] : 1.0 - fraction[2]);
          value += weight * paper_table[base + x * step[0] + y * step[1] + z * step[2] + k];
        }
        cube_root_xyz[kLayers * node + k] =
            std::cbrt(value * value * value + controls.surface_xyz[k]);
      }
    }
    outside_total += outside;
    not_reached_total += missed;
  });
  return {outside_total.load(), not_reached_total.load()};
}

}  // namespace alcedo::film_tables
