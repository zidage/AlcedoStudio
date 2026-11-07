//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_scene_tint.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

#include "edit/operators/basic/planckian_locus_table.hpp"
#include "edit/runtime/film/cie1931_cmf_table.hpp"

namespace alcedo::film_scene_tint {

auto TintBand(double wavelength_nm) -> double {
  const double z = (wavelength_nm - kTintBandCentreNm) / kTintBandSigmaNm;
  return std::exp(-0.5 * z * z);
}

auto WienLogPerMired(double wavelength_nm) -> double {
  return -kSecondRadiationConstantNmK * 1e-6 / wavelength_nm;
}

auto SpectrumXyz(std::span<const double> wavelengths_nm, std::span<const double> spectrum)
    -> std::array<double, 3> {
  if (wavelengths_nm.size() != spectrum.size()) {
    throw std::invalid_argument("Film print: a spectrum and its wavelengths differ in length");
  }
  const auto&           cmf  = cie1931::kCmf;
  const double          last = cie1931::kFirstNm + static_cast<double>(cmf.size() - 1);
  std::array<double, 3> xyz{};
  for (std::size_t w = 0; w < spectrum.size(); ++w) {
    const double nm = wavelengths_nm[w];
    if (!(nm >= cie1931::kFirstNm && nm <= last)) continue;
    const double position = (nm - cie1931::kFirstNm) / cie1931::kStepNm;
    const auto   low      = (std::min)(static_cast<std::size_t>(position), cmf.size() - 2);
    const double t        = position - static_cast<double>(low);
    const auto&  a        = cmf[low];
    const auto&  b        = cmf[low + 1];
    xyz[0] += spectrum[w] * (a.x_ + t * (b.x_ - a.x_));
    xyz[1] += spectrum[w] * (a.y_ + t * (b.y_ - a.y_));
    xyz[2] += spectrum[w] * (a.z_ + t * (b.z_ - a.z_));
  }
  return xyz;
}

auto Uv1960(const std::array<double, 3>& xyz) -> std::array<double, 2> {
  const double den = xyz[0] + 15.0 * xyz[1] + 3.0 * xyz[2];
  if (!(den > 0.0)) {
    throw std::runtime_error(
        "Film print: a light has no chromaticity (X + 15 Y + 3 Z is not above 0)");
  }
  return {4.0 * xyz[0] / den, 6.0 * xyz[1] / den};
}

auto Uv1960OfXy(const std::array<double, 2>& xy) -> std::array<double, 2> {
  const double den = -2.0 * xy[0] + 12.0 * xy[1] + 3.0;
  if (!(den > 0.0) || !std::isfinite(den)) {
    throw std::runtime_error("Film print: the chromaticity of the RAW white is not valid");
  }
  return {4.0 * xy[0] / den, 6.0 * xy[1] / den};
}

auto PlanckianOffset(const std::array<double, 2>& uv) -> LocusOffset {
  const auto& table   = planckian_locus::kUvTable;
  double      nearest = std::numeric_limits<double>::infinity();
  LocusOffset result;
  for (std::size_t i = 0; i + 1 < table.size(); ++i) {
    const double pu     = table[i].u_;
    const double pv     = table[i].v_;
    const double tu     = table[i + 1].u_ - pu;
    const double tv     = table[i + 1].v_ - pv;
    const double length = tu * tu + tv * tv;
    double       s      = ((uv[0] - pu) * tu + (uv[1] - pv) * tv) / length;
    s                   = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
    const double du     = uv[0] - (pu + s * tu);
    const double dv     = uv[1] - (pv + s * tv);
    const double d2     = du * du + dv * dv;
    if (d2 < nearest) {
      nearest           = d2;
      // The table rises in temperature: the tangent points to lower u and v, and (tv, -tu) points
      // to higher v, the green side.
      const double norm = std::sqrt(length);
      result.normal     = {tv / norm, -tu / norm};
      result.duv        = du * result.normal[0] + dv * result.normal[1];
    }
  }
  return result;
}

}  // namespace alcedo::film_scene_tint
