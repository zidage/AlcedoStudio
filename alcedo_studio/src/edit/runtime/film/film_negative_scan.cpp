//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_negative_scan.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <string>

#include "edit/runtime/film/film_scene_tint.hpp"
#include "edit/runtime/film/film_tables.hpp"

namespace alcedo::film_negative_scan {
namespace {

#define FILM_KERNEL_DOUBLE
#include "edit/runtime/film/film_print_math.h"
#undef FILM_KERNEL_DOUBLE

constexpr double kLog10Of2 = 0.30102999566398119521;
constexpr double                kGreyValue = 0.18;

/// CIE CAT02 (CIECAM02) XYZ to LMS, row major: the matrix of Color Grade white balance.
constexpr std::array<double, 9> kCat02     = {0.7328, 0.4296, -0.1624, -0.7036, 1.6975,
                                              0.0061, 0.0030, 0.0136,  0.9834};

using Matrix33                             = std::array<double, 9>;

auto Multiply(const Matrix33& m, const std::array<double, 3>& v) -> std::array<double, 3> {
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
          m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

/// The inverse of a 3 x 3 matrix by its adjugate.
auto Inverse(const Matrix33& m) -> Matrix33 {
  const Matrix33 adjugate = {
      m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
      m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
      m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
  const double determinant = m[0] * adjugate[0] + m[1] * adjugate[3] + m[2] * adjugate[6];
  if (!(std::abs(determinant) > 0.0) || !std::isfinite(determinant)) {
    throw std::runtime_error("Negative Scan: a colour matrix has no inverse");
  }
  Matrix33 inverse{};
  for (std::size_t i = 0; i < inverse.size(); ++i) inverse[i] = adjugate[i] / determinant;
  return inverse;
}

auto LessMean(std::array<double, 3> values) -> std::array<double, 3> {
  const double mean = (values[0] + values[1] + values[2]) / 3.0;
  for (auto& value : values) value -= mean;
  return values;
}

/// The aim of the inversion with rule 'medium_light_cast': D_aim - gamma light_cast, so that
/// (D - aim) / gamma = (D - D_aim) / gamma + light_cast.
auto ReadAim(const SelectionValues& values) -> std::array<double, 3> {
  std::array<double, 3> aim{};
  for (std::size_t k = 0; k < 3; ++k) {
    aim[k] = values.aim_density[k] - values.gamma[k] * values.light_cast[k];
  }
  return aim;
}

/// The layer exposure of a neutral of @p value under @p spectrum with the layer gain @p gain.
auto GreyExposure(const FilmNegativeView& film, const std::array<double, 3>& gain,
                  std::span<const double> spectrum, double value) -> std::array<double, 3> {
  const auto            count = spectrum.size();
  std::array<double, 3> q{};
  for (std::size_t k = 0; k < 3; ++k) {
    double sum = 0.0;
    for (std::size_t w = 0; w < count; ++w) {
      sum += value * spectrum[w] * film.exposure_weights[k * count + w];
    }
    q[k] = gain[k] * sum;
  }
  return q;
}

/// numpy.interp on a uniform grid: `start + step k` for k < @p length, the end values outside.
auto InterpUniform(std::span<const double> values, std::int64_t length, double start, double step,
                   double x) -> double {
  const double position = (x - start) / step;
  if (!(position > 0.0)) return values[0];
  const auto last = static_cast<double>(length - 1);
  if (position >= last) return values[static_cast<std::size_t>(length - 1)];
  const auto   cell = static_cast<std::int64_t>(std::floor(position));
  const double t    = position - static_cast<double>(cell);
  const auto   a    = values[static_cast<std::size_t>(cell)];
  const auto   b    = values[static_cast<std::size_t>(cell + 1)];
  return a + t * (b - a);
}

auto Median(std::vector<double> values) -> double {
  const auto count = values.size();
  const auto upper = values.begin() + static_cast<std::ptrdiff_t>(count / 2);
  std::nth_element(values.begin(), upper, values.end());
  if (count % 2 == 1) return *upper;
  const auto lower = *std::max_element(values.begin(), upper);
  return 0.5 * (lower + *upper);
}

/// numpy.quantile with the default method 'linear'.
auto Quantile(std::vector<double> values, double q) -> double {
  std::sort(values.begin(), values.end());
  const double position = q * static_cast<double>(values.size() - 1);
  const auto   lower    = static_cast<std::size_t>(std::floor(position));
  const auto   upper    = (std::min)(lower + 1, values.size() - 1);
  const double t        = position - static_cast<double>(lower);
  return values[lower] + t * (values[upper] - values[lower]);
}

auto CentredMedianCast(const std::vector<std::array<double, 3>>& u,
                       const std::vector<std::size_t>&            rows) -> std::array<double, 3> {
  std::array<double, 3> cast{};
  for (std::size_t k = 0; k < 3; ++k) {
    std::vector<double> channel;
    channel.reserve(rows.size());
    for (const auto row : rows) channel.push_back(u[row][k]);
    cast[k] = Median(std::move(channel));
  }
  const double mean = (cast[0] + cast[1] + cast[2]) / 3.0;
  for (auto& value : cast) value -= mean;
  return cast;
}

auto SquaredDistance(const std::array<double, 3>& a, const std::array<double, 3>& b) -> double {
  return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
         (a[2] - b[2]) * (a[2] - b[2]);
}

auto Norm(const std::array<double, 3>& a) -> double {
  return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

/// Pillow `precompute_coeffs` with the BOX filter: per output pixel the first input pixel and the
/// normalised weights of the input pixels.
struct BoxCoefficients {
  std::vector<std::int64_t> first;
  std::vector<std::int64_t> count;
  std::vector<double>       weights;  ///< (output, kernel size)
  std::int64_t              kernel = 0;
};

auto BoxCoefficientsOf(std::int64_t input, std::int64_t output) -> BoxCoefficients {
  const double scale       = static_cast<double>(input) / static_cast<double>(output);
  const double filterscale = (std::max)(scale, 1.0);
  const double support     = 0.5 * filterscale;
  BoxCoefficients result;
  result.kernel = static_cast<std::int64_t>(std::ceil(support)) * 2 + 1;
  result.first.resize(static_cast<std::size_t>(output));
  result.count.resize(static_cast<std::size_t>(output));
  result.weights.assign(static_cast<std::size_t>(output * result.kernel), 0.0);
  for (std::int64_t out = 0; out < output; ++out) {
    const double center = (static_cast<double>(out) + 0.5) * scale;
    auto         xmin   = static_cast<std::int64_t>(center - support + 0.5);
    xmin                = (std::max)(xmin, std::int64_t{0});
    auto xmax           = static_cast<std::int64_t>(center + support + 0.5);
    xmax                = (std::min)(xmax, input) - xmin;
    double total        = 0.0;
    auto*  weights      = result.weights.data() + out * result.kernel;
    for (std::int64_t x = 0; x < xmax; ++x) {
      const double position = (static_cast<double>(x + xmin) - center + 0.5) / filterscale;
      const double weight   = (position > -0.5 && position <= 0.5) ? 1.0 : 0.0;
      weights[x]            = weight;
      total += weight;
    }
    for (std::int64_t x = 0; x < xmax; ++x) {
      if (total != 0.0) weights[x] /= total;
    }
    result.first[static_cast<std::size_t>(out)] = xmin;
    result.count[static_cast<std::size_t>(out)] = xmax;
  }
  return result;
}

void RequireRange(double value, double low, double high, const char* name) {
  if (!(value >= low && value <= high)) {
    throw std::runtime_error(std::string("Negative Scan: the scanner ") + name + " " +
                             std::to_string(value) + " is outside " + std::to_string(low) +
                             " to " + std::to_string(high));
  }
}

}  // namespace

auto DefaultScanner(const FilmNegativeScanConstants& constants) -> Scanner {
  return {constants.centres_nm, constants.width_nm, constants.default_light == "equal_energy"};
}

auto ScannerOf(const FilmNegativeScanParams& params) -> Scanner {
  if (!IsFilmIdentifier(kFilmScannerLightIds, params.scanner_light)) {
    throw std::runtime_error("Negative Scan: unknown scanner light '" + params.scanner_light + "'");
  }
  return {{static_cast<double>(params.scanner_red_nm), static_cast<double>(params.scanner_green_nm),
           static_cast<double>(params.scanner_blue_nm)},
          static_cast<double>(params.scanner_width_nm),
          params.scanner_light == "equal_energy"};
}

auto MakeScannerWeights(const Scanner& scanner, const FilmNegativeScanConstants& constants,
                        std::span<const double> wavelengths_nm) -> ScannerWeights {
  // Lab ScannerSetup.validate.
  RequireRange(scanner.width_nm, 5.0, 150.0, "band width");
  const auto& c = scanner.centres_nm;
  if (!(380.0 <= c[2] && c[2] < c[1] && c[1] < c[0] && c[0] <= 760.0)) {
    throw std::runtime_error(
        "Negative Scan: the scanner centres need 380 <= blue < green < red <= 760 nm");
  }
  const auto   count = wavelengths_nm.size();
  const double sigma = scanner.width_nm / (2.0 * std::sqrt(2.0 * std::log(2.0)));
  std::vector<double> bands(3 * count);
  std::int64_t        first = -1;
  std::int64_t        last  = -1;
  for (std::size_t w = 0; w < count; ++w) {
    bool kept = false;
    for (std::size_t k = 0; k < 3; ++k) {
      const double z   = (wavelengths_nm[w] - c[k]) / sigma;
      bands[k * count + w] = std::exp(-0.5 * z * z);
      kept |= bands[k * count + w] >= constants.band_cut;
    }
    if (kept) {
      if (first < 0) first = static_cast<std::int64_t>(w);
      last = static_cast<std::int64_t>(w);
    }
  }
  if (first < 0) {
    throw std::runtime_error("Negative Scan: no wavelength of the bundle is in a scanner band");
  }
  ScannerWeights result;
  result.first_node = first;
  result.nodes      = last - first + 1;
  result.weights.assign(3 * count, 0.0);
  for (auto w = static_cast<std::size_t>(first); w <= static_cast<std::size_t>(last); ++w) {
    const double lambda = wavelengths_nm[w];
    const double lamp   = scanner.equal_energy
                              ? 1.0
                              : std::pow(lambda, -5.0) /
                                  std::expm1(constants.planck_c2_nm_k / (lambda * constants.lamp_kelvin));
    for (std::size_t k = 0; k < 3; ++k) {
      const double band = bands[k * count + w];
      result.weights[k * count + w] = (band >= constants.band_cut ? band : 0.0) * lamp;
    }
  }
  return result;
}

auto BaseDensity(const ScannerWeights& weights, std::span<const double> base)
    -> std::array<double, 3> {
  const auto            count = base.size();
  std::array<double, 3> density{};
  for (std::size_t k = 0; k < 3; ++k) {
    double sum         = 0.0;
    double transmitted = 0.0;
    for (std::size_t w = 0; w < count; ++w) {
      const double weight = weights.weights[k * count + w];
      sum += weight;
      transmitted += weight * std::pow(10.0, -base[w]);
    }
    density[k] = std::log10(sum) - std::log10(transmitted);
  }
  return density;
}

auto ReadScannerTable(const ScannerTableView& table, const FilmNegativeView& negative,
                      const std::array<double, 3>& record) -> std::array<double, 3> {
  const auto edge = table.edge;
  std::array<std::int64_t, 3> low{};
  std::array<double, 3>       t{};
  for (std::size_t k = 0; k < 3; ++k) {
    const double start = negative.density_grid_start[k];
    const double step  = (negative.density_grid_stop[k] - start) / static_cast<double>(edge - 1);
    double position    = (record[k] - start) / step;
    position           = std::clamp(position, 0.0, static_cast<double>(edge - 1));
    low[k]             = (std::min)(static_cast<std::int64_t>(std::floor(position)), edge - 2);
    t[k]               = position - static_cast<double>(low[k]);
  }
  std::array<double, 3> out{};
  for (int corner = 0; corner < 8; ++corner) {
    const std::int64_t r = low[0] + (corner & 1);
    const std::int64_t g = low[1] + ((corner >> 1) & 1);
    const std::int64_t b = low[2] + ((corner >> 2) & 1);
    const double weight  = ((corner & 1) ? t[0] : 1.0 - t[0]) *
                          (((corner >> 1) & 1) ? t[1] : 1.0 - t[1]) *
                          (((corner >> 2) & 1) ? t[2] : 1.0 - t[2]);
    const auto index = static_cast<std::size_t>(((r * edge + g) * edge + b) * 3);
    for (std::size_t k = 0; k < 3; ++k) out[k] += weight * table.values[index + k];
  }
  return out;
}

auto PointRecord(const FilmNegativeView& negative, const std::array<double, 3>& q)
    -> std::array<double, 3> {
  std::array<double, 3> log_h{};
  std::array<double, 3> amounts{};
  const auto&           curves = negative.curves;
  for (std::size_t k = 0; k < 3; ++k) {
    log_h[k]   = negative.criterion_log_h[k] + std::log10((std::max)(q[k], 1e-300));
    const auto row = curves.density.subspan(k * static_cast<std::size_t>(curves.stored_length),
                                            static_cast<std::size_t>(curves.stored_length));
    amounts[k] = InterpUniform(row, curves.lengths[k], curves.log_h_start[k], curves.log_h_step,
                               log_h[k]);
  }
  const auto& inhibition = negative.inhibition;
  if (!inhibition.Present()) return amounts;
  for (int step = 0; step < kPointInhibitionSteps; ++step) {
    std::array<double, 3> next{};
    for (std::size_t k = 0; k < 3; ++k) {
      const double reduced = log_h[k] - (inhibition.k[k * 3] * amounts[0] +
                                         inhibition.k[k * 3 + 1] * amounts[1] +
                                         inhibition.k[k * 3 + 2] * amounts[2]);
      const auto row = inhibition.free_values.subspan(
          k * static_cast<std::size_t>(inhibition.stored_length),
          static_cast<std::size_t>(inhibition.stored_length));
      next[k] = InterpUniform(row, inhibition.lengths[k], inhibition.log_h_start[k],
                              inhibition.log_h_step, reduced);
    }
    amounts = next;
  }
  return amounts;
}

auto GreyDensity(const FilmStockBundle& bundle, std::string_view negative,
                 const ScannerTableView& table, const std::array<double, 3>& base_density,
                 const std::array<double, 3>& gain, std::span<const double> spectrum, double value)
    -> std::array<double, 3> {
  const auto film = bundle.Negative(negative);
  auto       density =
      ReadScannerTable(table, film, PointRecord(film, GreyExposure(film, gain, spectrum, value)));
  for (std::size_t k = 0; k < 3; ++k) density[k] -= base_density[k];
  return density;
}

auto DeriveMediumGamma(const FilmStockBundle& bundle, const FilmScanMedium& medium,
                       const ScannerWeights& weights, const ScannerTableOf& table_of)
    -> std::array<double, 3> {
  std::array<double, 3> sum{};
  for (std::size_t s = 0; s < medium.pool.size(); ++s) {
    const auto& stock   = medium.pool[s];
    const auto  film    = bundle.Negative(stock);
    const auto  base    = BaseDensity(weights, film.base);
    const auto  gain    = bundle.LayerGain(stock, medium.pool_scene_lights[s]);
    const auto  spectrum = bundle.LightSpectrum(bundle.ScanBalanceLight(stock));
    const auto  table    = table_of(stock);
    const auto  high = GreyDensity(bundle, stock, table, base, gain, spectrum, 0.36);
    const auto  low  = GreyDensity(bundle, stock, table, base, gain, spectrum, 0.09);
    for (std::size_t k = 0; k < 3; ++k) sum[k] += (high[k] - low[k]) / (2.0 * kLog10Of2);
  }
  for (auto& value : sum) {
    value /= static_cast<double>(medium.pool.size());
    // The light axis and x divide by gamma, as the stored gamma that the bundle checks.
    if (!(value > 0.0) || !std::isfinite(value)) {
      throw std::runtime_error("Negative Scan: the gamma of the medium '" + medium.id +
                               "' for this scanner is not above 0");
    }
  }
  return sum;
}

auto DeriveSelectionValues(const FilmStockBundle& bundle, std::string_view negative,
                           const FilmNegativeScanSetup& setup, const ScannerWeights& weights,
                           const ScannerTableView& table, const std::array<double, 3>& gamma)
    -> SelectionValues {
  const auto&     axis_lights = bundle.NegativeScanConstants().axis_lights;
  SelectionValues values;
  values.gamma        = gamma;
  values.base_density = BaseDensity(weights, bundle.Negative(negative).base);
  const auto grey     = [&](std::string_view light) {
    return GreyDensity(bundle, negative, table, values.base_density, setup.layer_gain,
                           bundle.LightSpectrum(light), 0.18);
  };
  values.aim_density = grey(setup.balance_light);
  const auto from     = grey(axis_lights[0]);
  const auto to       = grey(axis_lights[1]);
  for (std::size_t k = 0; k < 3; ++k) values.light_axis[k] = (to[k] - from[k]) / gamma[k];
  return values;
}

auto StoredSelectionValues(const FilmNegativeScanSetup& setup) -> SelectionValues {
  return {setup.base_density, setup.aim_density, setup.light_axis, setup.gamma, {}, {}};
}

auto ScanDensityOfExposure(const FilmNegativeView& negative, const ScannerWeights& weights,
                           const std::array<double, 3>& q) -> std::array<double, 3> {
  const auto count = negative.base.size();
  if (weights.weights.size() != 3 * count || negative.dyes.size() != 3 * count) {
    throw std::runtime_error(
        "Negative Scan: the scanner weights and the negative have different wavelengths");
  }
  const auto                      record = PointRecord(negative, q);
  const film_tables::SpectralBand band{static_cast<std::int64_t>(count), negative.base.data(),
                                       negative.dyes.data(), weights.weights.data()};
  std::array<double, 3>           response{};
  film_tables::Log10BandResponse(band, record.data(), 1, response.data(), 1);
  std::array<double, 3> density{};
  for (std::size_t k = 0; k < 3; ++k) {
    double sum = 0.0;
    for (std::size_t w = 0; w < count; ++w) sum += weights.weights[k * count + w];
    density[k] = std::log10(sum) - response[k];
  }
  return density;
}

auto ColorimetricLightCast(const FilmStockBundle& bundle, std::span<const double> light,
                           std::span<const double> balance) -> std::array<double, 3> {
  const auto  wavelengths = bundle.WavelengthsNm();
  const auto& matrix      = bundle.PointConstants().ap1_to_xyz;
  Matrix33    ap1_to_xyz{};
  std::copy_n(matrix.begin(), ap1_to_xyz.size(), ap1_to_xyz.begin());
  const auto xyz_to_ap1  = Inverse(ap1_to_xyz);
  const auto lms_to_xyz  = Inverse(kCat02);
  const auto lms_white   = Multiply(kCat02, Multiply(ap1_to_xyz, {1.0, 1.0, 1.0}));
  const auto lms_balance = Multiply(kCat02, film_scene_tint::SpectrumXyz(wavelengths, balance));
  // The AP1 of a light after the adaptation. The balance light goes through the same steps and
  // the cast is the difference, so @p balance itself gives a cast of exactly 0.
  const auto adapted     = [&](std::span<const double> spectrum) {
    auto lms = Multiply(kCat02, film_scene_tint::SpectrumXyz(wavelengths, spectrum));
    for (std::size_t k = 0; k < 3; ++k) lms[k] *= lms_white[k] / lms_balance[k];
    const auto ap1 = Multiply(xyz_to_ap1, Multiply(lms_to_xyz, lms));
    for (const double value : ap1) {
      if (!(value > 0.0) || !std::isfinite(value)) {
        throw std::runtime_error(
            "Negative Scan: a scene light adapted to the balance light has an AP1 value at or "
                "below 0");
      }
    }
    return ap1;
  };
  const auto            lit       = adapted(light);
  const auto            reference = adapted(balance);
  std::array<double, 3> cast{};
  for (std::size_t k = 0; k < 3; ++k) cast[k] = std::log10(lit[k]) - std::log10(reference[k]);
  return LessMean(cast);
}

auto MediumFilmCast(const FilmStockBundle& bundle, const FilmScanMedium& medium,
                    const ScannerWeights& weights, const std::array<double, 3>& gamma,
                    std::span<const double> light, std::span<const double> balance)
    -> std::array<double, 3> {
  if (medium.pool.empty()) {
    throw std::runtime_error("Negative Scan: the medium '" + medium.id + "' has no pool");
  }
  std::array<double, 3> sum{};
  for (std::size_t s = 0; s < medium.pool.size(); ++s) {
    const auto film = bundle.Negative(medium.pool[s]);
    const auto gain = bundle.LayerGain(medium.pool[s], medium.pool_scene_lights[s]);
    const auto lit =
        ScanDensityOfExposure(film, weights, GreyExposure(film, gain, light, kGreyValue));
    const auto reference =
        ScanDensityOfExposure(film, weights, GreyExposure(film, gain, balance, kGreyValue));
    for (std::size_t k = 0; k < 3; ++k) sum[k] += (lit[k] - reference[k]) / gamma[k];
  }
  for (auto& value : sum) value /= static_cast<double>(medium.pool.size());
  return LessMean(sum);
}

auto WithMediumLightCast(const SelectionValues& values, const FilmStockBundle& bundle,
                         std::string_view negative, const ScannerWeights& weights,
                         std::span<const double> scene_spectrum) -> SelectionValues {
  const auto& medium  = bundle.ScanMedium(negative);
  const auto  balance = bundle.LightSpectrum(bundle.ScanBalanceLight(negative));
  const auto  term    = [&](std::span<const double> light) {
    const auto colorimetric = ColorimetricLightCast(bundle, light, balance);
    const auto film = MediumFilmCast(bundle, medium, weights, values.gamma, light, balance);
    std::array<double, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) out[k] = colorimetric[k] - film[k];
    return out;
  };
  const auto&     axis_lights = bundle.NegativeScanConstants().axis_lights;
  const auto      from        = term(bundle.LightSpectrum(axis_lights[0]));
  const auto      to          = term(bundle.LightSpectrum(axis_lights[1]));
  SelectionValues result      = values;
  result.light_cast           = term(scene_spectrum);
  for (std::size_t k = 0; k < 3; ++k) result.light_axis_cast[k] = to[k] - from[k];
  return result;
}

auto WithoutMediumLightCast(const SelectionValues& values) -> SelectionValues {
  SelectionValues result = values;
  result.light_cast      = {};
  result.light_axis_cast = {};
  return result;
}

auto ReadLightAxis(const SelectionValues& values) -> std::array<double, 3> {
  std::array<double, 3> axis{};
  for (std::size_t k = 0; k < 3; ++k) axis[k] = values.light_axis[k] + values.light_axis_cast[k];
  return axis;
}

auto ManualShift(const FilmNegativeScanParams& params) -> std::array<double, 3> {
  const double exposure = static_cast<double>(params.exposure_stops) * kLog10Of2;
  return {exposure - static_cast<double>(params.cyan), exposure - static_cast<double>(params.magenta),
          exposure - static_cast<double>(params.yellow)};
}

auto MakeRenderTerms(const SelectionValues& values, const FilmNegativeScanParams& params,
                     const std::array<double, 3>& auto_shift) -> RenderTerms {
  const auto manual = ManualShift(params);
  double     shift[3];
  for (std::size_t k = 0; k < 3; ++k) shift[k] = auto_shift[k] + manual[k];
  const auto             aim = ReadAim(values);
  FilmKernelNegativeScan terms{};
  const int              valid = FilmKernelNegativeScanTerms(
      values.base_density.data(), aim.data(), values.gamma.data(),
      static_cast<double>(params.contrast), shift, static_cast<double>(params.black), &terms);
  if (valid == 0) {
    throw std::runtime_error(
        "Negative Scan: with Black at film base " + std::to_string(params.black) +
        " the film base is at or above the grey; lower the black share or raise the contrast");
  }
  RenderTerms result;
  for (std::size_t k = 0; k < 3; ++k) {
    result.slope[k]  = terms.slope[k];
    result.offset[k] = terms.offset[k];
    result.scale[k]  = terms.scale[k];
    result.floor[k]  = terms.floor[k];
  }
  return result;
}

auto MakePrescanTerms(const SelectionValues& values, const FilmNegativeScanParams& params)
    -> RenderTerms {
  const auto  aim = ReadAim(values);
  RenderTerms result;
  for (std::size_t k = 0; k < 3; ++k) {
    result.slope[k]  = static_cast<double>(params.contrast) / values.gamma[k];
    result.offset[k] = -result.slope[k] * (values.base_density[k] + aim[k]);
    result.scale[k]  = 1.0;
    result.floor[k]  = 0.0;
  }
  return result;
}

auto PrescanShape(std::int64_t rows, std::int64_t columns) -> std::array<std::int64_t, 2> {
  const double scale =
      static_cast<double>(kAutoColour.long_edge) / static_cast<double>((std::max)(rows, columns));
  if (scale >= 1.0) return {rows, columns};
  // Python round(): half to even, as std::nearbyint in the default rounding mode.
  return {(std::max)(std::int64_t{1}, static_cast<std::int64_t>(std::nearbyint(
                                          static_cast<double>(rows) * scale))),
          (std::max)(std::int64_t{1}, static_cast<std::int64_t>(std::nearbyint(
                                          static_cast<double>(columns) * scale)))};
}

auto ReduceFrame(const Frame& frame) -> Frame {
  const auto shape = PrescanShape(frame.rows, frame.columns);
  Frame      out;
  out.rows    = shape[0];
  out.columns = shape[1];
  if (out.rows == frame.rows && out.columns == frame.columns) {
    // Lab reduce_frame: a frame no larger than the prescan is read as it is, in float64.
    out.values = frame.values;
    return out;
  }
  // Pillow ImagingResample: the horizontal pass, then the vertical pass, each pass skipped when its
  // size does not change, each stored as float32.
  const bool horizontal = out.columns != frame.columns;
  const bool vertical   = out.rows != frame.rows;
  const auto cols       = BoxCoefficientsOf(frame.columns, out.columns);
  const auto rows       = BoxCoefficientsOf(frame.rows, out.rows);
  out.values.assign(static_cast<std::size_t>(out.rows * out.columns * 3), 0.0);
  for (std::size_t k = 0; k < 3; ++k) {
    std::vector<float> source(static_cast<std::size_t>(frame.rows * frame.columns));
    for (std::size_t i = 0; i < source.size(); ++i) {
      source[i] = static_cast<float>(frame.values[i * 3 + k]);
    }
    std::int64_t       width = frame.columns;
    std::vector<float> middle;
    if (horizontal) {
      middle.resize(static_cast<std::size_t>(frame.rows * out.columns));
      for (std::int64_t y = 0; y < frame.rows; ++y) {
        for (std::int64_t x = 0; x < out.columns; ++x) {
          const auto* weights = cols.weights.data() + x * cols.kernel;
          const auto  first   = cols.first[static_cast<std::size_t>(x)];
          double      sum     = 0.0;
          for (std::int64_t i = 0; i < cols.count[static_cast<std::size_t>(x)]; ++i) {
            sum += static_cast<double>(source[static_cast<std::size_t>(y * frame.columns + first + i)]) *
                   weights[i];
          }
          middle[static_cast<std::size_t>(y * out.columns + x)] = static_cast<float>(sum);
        }
      }
      width = out.columns;
    } else {
      middle = std::move(source);
    }
    for (std::int64_t y = 0; y < out.rows; ++y) {
      for (std::int64_t x = 0; x < width; ++x) {
        float value = 0.0f;
        if (vertical) {
          const auto* weights = rows.weights.data() + y * rows.kernel;
          const auto  first   = rows.first[static_cast<std::size_t>(y)];
          double      sum     = 0.0;
          for (std::int64_t i = 0; i < rows.count[static_cast<std::size_t>(y)]; ++i) {
            sum += static_cast<double>(middle[static_cast<std::size_t>((first + i) * width + x)]) *
                   weights[i];
          }
          value = static_cast<float>(sum);
        } else {
          value = middle[static_cast<std::size_t>(y * width + x)];
        }
        out.values[static_cast<std::size_t>((y * out.columns + x) * 3) + k] = value;
      }
    }
  }
  return out;
}

auto PrescanAxisOf(std::int64_t input, std::int64_t output) -> PrescanAxis {
  if (input <= 0 || output <= 0) {
    throw std::invalid_argument("Negative Scan: the prescan has an empty axis");
  }
  PrescanAxis axis;
  axis.first.resize(static_cast<std::size_t>(output));
  axis.count.resize(static_cast<std::size_t>(output));
  if (input == output) {
    // Pillow skips a pass that keeps its size: each value stays as it is.
    for (std::int64_t out = 0; out < output; ++out) {
      axis.first[static_cast<std::size_t>(out)] = static_cast<std::int32_t>(out);
      axis.count[static_cast<std::size_t>(out)] = 1;
    }
    return axis;
  }
  const auto coefficients = BoxCoefficientsOf(input, output);
  for (std::int64_t out = 0; out < output; ++out) {
    const auto*  weights = coefficients.weights.data() + out * coefficients.kernel;
    const auto   taps    = coefficients.count[static_cast<std::size_t>(out)];
    std::int64_t begin   = 0;
    while (begin < taps && weights[begin] == 0.0) ++begin;
    std::int64_t end = taps;
    while (end > begin && weights[end - 1] == 0.0) --end;
    for (std::int64_t i = begin; i < end; ++i) {
      // The window form needs one weight for all its pixels.
      if (weights[i] != weights[begin]) {
        throw std::logic_error("Negative Scan: a prescan window has unequal weights");
      }
    }
    axis.first[static_cast<std::size_t>(out)] =
        static_cast<std::int32_t>(coefficients.first[static_cast<std::size_t>(out)] + begin);
    axis.count[static_cast<std::size_t>(out)] = static_cast<std::int32_t>(end - begin);
  }
  return axis;
}

auto PrescanPlanOf(std::int64_t frame_rows, std::int64_t frame_columns, std::int64_t render_rows,
                   std::int64_t render_columns) -> PrescanPlan {
  if (frame_rows <= 0 || frame_columns <= 0 || render_rows <= 0 || render_columns <= 0) {
    throw std::invalid_argument("Negative Scan: the prescan has an empty frame or render");
  }
  const auto  shape   = PrescanShape(frame_rows, frame_columns);
  PrescanPlan plan;
  plan.rows           = shape[0];
  plan.columns        = shape[1];
  const auto columns  = PrescanAxisOf(render_columns, shape[1]);
  const auto rows     = PrescanAxisOf(render_rows, shape[0]);
  for (const auto* part : {&columns.first, &columns.count, &rows.first, &rows.count}) {
    plan.windows.insert(plan.windows.end(), part->begin(), part->end());
  }
  plan.windows_key = ContentHash{}
                         .MixText("film scan prescan windows")
                         .MixU64(static_cast<std::uint64_t>(render_columns))
                         .MixU64(static_cast<std::uint64_t>(render_rows))
                         .MixU64(static_cast<std::uint64_t>(plan.columns))
                         .MixU64(static_cast<std::uint64_t>(plan.rows))
                         .Key();
  return plan;
}

auto AutoColourOfDensityPrescan(std::span<const float> density_rgba, std::int64_t rows,
                                std::int64_t columns, const SelectionValues& values,
                                const FilmNegativeScanParams& params) -> AutoColourResult {
  const auto pixels = static_cast<std::size_t>(rows * columns);
  if (rows <= 0 || columns <= 0 || density_rgba.size() != pixels * 4U) {
    throw std::runtime_error("Negative Scan: the prescan of the scan density has " +
                             std::to_string(density_rgba.size()) + " values, not " +
                             std::to_string(pixels * 4U));
  }
  const auto terms = MakePrescanTerms(values, params);
  Frame      prescan{{}, rows, columns};
  prescan.values.resize(pixels * 3U);
  for (std::size_t p = 0; p < pixels; ++p) {
    for (std::size_t k = 0; k < 3; ++k) {
      prescan.values[p * 3 + k] =
          terms.slope[k] * static_cast<double>(density_rgba[p * 4 + k]) + terms.offset[k];
    }
  }
  return AutoColourShift(prescan, ReadLightAxis(values), params.auto_colour,
                         static_cast<double>(params.auto_strength),
                         static_cast<double>(params.auto_density));
}

auto AutoColourShift(const Frame& prescan, const std::array<double, 3>& light_axis,
                     std::string_view mode, double strength, double density_strength)
    -> AutoColourResult {
  AutoColourResult result;
  const auto       mode_index = static_cast<std::size_t>(
      std::find(kFilmScanAutoIds.begin(), kFilmScanAutoIds.end(), mode) - kFilmScanAutoIds.begin());
  if (mode_index >= kFilmScanAutoIds.size()) {
    throw std::runtime_error("Negative Scan: unknown auto colour mode '" + std::string(mode) + "'");
  }
  if (mode == "off") return result;
  const auto&                        c      = kAutoColour;
  const auto                         pixels = static_cast<std::size_t>(prescan.rows * prescan.columns);
  std::vector<double>                level;
  std::vector<std::array<double, 3>> u;
  for (std::size_t p = 0; p < pixels; ++p) {
    const double r    = prescan.values[p * 3];
    const double g    = prescan.values[p * 3 + 1];
    const double b    = prescan.values[p * 3 + 2];
    const double mean = (r + g + b) / 3.0;
    if (mean > c.valid_low && mean < c.valid_high && std::isfinite(mean)) {
      level.push_back(mean);
      u.push_back({r - mean, g - mean, b - mean});
    }
  }
  result.prescan_pixels = static_cast<std::int64_t>(level.size());
  if (level.size() < static_cast<std::size_t>(c.minimum_pixels)) {
    throw std::runtime_error("Negative Scan: rule frame_auto_colour has only " +
                             std::to_string(level.size()) +
                             " prescan pixels inside the valid range");
  }
  std::vector<std::size_t> all(level.size());
  std::iota(all.begin(), all.end(), std::size_t{0});
  auto       cast    = CentredMedianCast(u, all);
  const auto sigma2  = c.sigma * c.sigma;
  const auto weights = [&](const std::array<double, 3>& centre) {
    std::vector<double> w(u.size());
    for (std::size_t i = 0; i < u.size(); ++i) w[i] = std::exp(-SquaredDistance(u[i], centre) / sigma2);
    return w;
  };
  for (int iteration = 0; iteration < c.iterations; ++iteration) {
    const auto            w     = weights(cast);
    const double          total = std::accumulate(w.begin(), w.end(), 0.0);
    if (total <= 1e-9) break;
    std::array<double, 3> next{};
    for (std::size_t i = 0; i < u.size(); ++i) {
      for (std::size_t k = 0; k < 3; ++k) next[k] += w[i] * u[i][k];
    }
    for (auto& value : next) value /= total;
    cast = next;
  }
  result.near_neutral_cast = cast;
  const double             threshold = Quantile(level, 1.0 - c.highlight_share);
  std::vector<std::size_t> bright;
  for (std::size_t i = 0; i < level.size(); ++i) {
    if (level[i] >= threshold) bright.push_back(i);
  }
  result.white_patch_cast = CentredMedianCast(u, bright);
  cast = Norm(result.white_patch_cast) < Norm(result.near_neutral_cast) ? result.white_patch_cast
                                                                         : result.near_neutral_cast;
  result.cast = cast;
  std::array<double, 3> axis = light_axis;
  const double          axis_mean = (axis[0] + axis[1] + axis[2]) / 3.0;
  for (auto& value : axis) value -= axis_mean;
  const double axis_norm = Norm(axis);
  for (auto& value : axis) value /= axis_norm;
  const double along = cast[0] * axis[0] + cast[1] * axis[1] + cast[2] * axis[2];
  const double target = c.target[mode_index];
  const auto   w      = weights(cast);
  double       weight_sum = 0.0;
  double       level_sum  = 0.0;
  for (std::size_t i = 0; i < level.size(); ++i) {
    weight_sum += w[i] + 1e-12;
    level_sum += (w[i] + 1e-12) * level[i];
  }
  const double mean_level = level_sum / weight_sum;
  for (std::size_t k = 0; k < 3; ++k) {
    const double across = cast[k] - along * axis[k];
    result.shift[k]     = -(strength * (along - target)) * axis[k] -
                      strength * c.cross_share * across - density_strength * mean_level;
  }
  return result;
}

}  // namespace alcedo::film_negative_scan
