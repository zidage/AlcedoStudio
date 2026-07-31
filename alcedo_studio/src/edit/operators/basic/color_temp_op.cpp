//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/basic/color_temp_op.hpp"
#include "edit/operators/basic/planckian_locus_table.hpp"

#include <iostream>
#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace alcedo {
namespace {
constexpr double kCalibrationLowCCT   = 2856.0;
constexpr double kCalibrationHighCCT  = 6504.0;
constexpr double kCustomCCTMin        = 2000.0;
constexpr double kCustomCCTMax        = 15000.0;
constexpr double kCustomTintMin       = -150.0;
constexpr double kCustomTintMax       = 150.0;
constexpr double kTintScale           = 3000.0;  // Adobe DNG SDK temperature/tint model.
constexpr double kDeterminantEpsilon  = 1e-10;
constexpr double kValueEpsilon        = 1e-10;
constexpr double kAsShotSolveEpsilon  = 1e-8;
constexpr int    kAsShotSolveMaxIter  = 16;
constexpr double kOhnoNearLocusDuv    = 0.002;
constexpr double kD50X                = 0.34567;
constexpr double kD50Y                = 0.35850;
constexpr double kD60X                = 0.32168;
constexpr double kD60Y                = 0.33767;
// ACEScg (AP1) white is D60. This is XYZ(D60) -> AP1 in column-vector form.
const cv::Matx33d kXyzD60ToAp1(1.6410233797, -0.3248032942, -0.2364246952, -0.6636628587,
                               1.6153315917, 0.0167563477, 0.0117218943, -0.0082844420,
                               0.9883948585);

auto ClampFinite(double value, double min_value, double max_value) -> double {
  if (!std::isfinite(value)) {
    return min_value;
  }
  return std::clamp(value, min_value, max_value);
}

auto IsFiniteMatrix(const cv::Matx33d& m) -> bool {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(m(r, c))) {
        return false;
      }
    }
  }
  return true;
}

void HashCombine(std::uint64_t& seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

auto FloatHashBits(float value) -> std::uint64_t {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

void HashFloatArray(std::uint64_t& seed, const float* values, int count) {
  for (int i = 0; i < count; ++i) {
    HashCombine(seed, FloatHashBits(values[i]));
  }
}

auto BuildRuntimeCacheKey(const OperatorParams& params, ColorTempMode mode, float custom_cct,
                          float custom_tint) -> std::uint64_t {
  std::uint64_t key = 0xcbf29ce484222325ULL;
  HashCombine(key, static_cast<std::uint64_t>(params.color_temp_enabled_));
  HashCombine(key, static_cast<std::uint64_t>(params.raw_runtime_valid_));
  HashCombine(key, static_cast<std::uint64_t>(mode));
  HashCombine(key, FloatHashBits(custom_cct));
  HashCombine(key, FloatHashBits(custom_tint));
  HashCombine(key, static_cast<std::uint64_t>(params.raw_decode_input_space_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_make_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_model_)));
  HashFloatArray(key, params.raw_pre_mul_, 3);
  HashFloatArray(key, params.raw_cam_xyz_, 9);
  HashCombine(key, static_cast<std::uint64_t>(params.raw_color_matrices_valid_));
  if (params.raw_color_matrices_valid_) {
    for (double value : params.raw_color_matrix_1_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
    for (double value : params.raw_color_matrix_2_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
    HashCombine(key, static_cast<std::uint64_t>(params.raw_calibration_illuminants_valid_));
    HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(params.raw_color_matrix_1_cct_)));
    HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(params.raw_color_matrix_2_cct_)));
  }
  HashCombine(key, static_cast<std::uint64_t>(params.raw_forward_matrices_valid_));
  if (params.raw_forward_matrices_valid_) {
    for (double value : params.raw_forward_matrix_1_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
    for (double value : params.raw_forward_matrix_2_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
  }
  HashCombine(key, static_cast<std::uint64_t>(params.raw_as_shot_neutral_valid_));
  if (params.raw_as_shot_neutral_valid_) {
    for (double value : params.raw_as_shot_neutral_) {
      HashCombine(key, static_cast<std::uint64_t>(std::hash<double>{}(value)));
    }
  }
  if (mode == ColorTempMode::AS_SHOT) {
    HashFloatArray(key, params.raw_cam_mul_, 3);
  }
  return key;
}

auto MatrixFromArray9(const double m[9]) -> cv::Matx33d {
  return cv::Matx33d(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
}

auto HasValidCamXyz(const float m[9]) -> bool {
  double sum = 0.0;
  for (int i = 0; i < 9; ++i) {
    if (!std::isfinite(m[i])) {
      return false;
    }
    sum += std::abs(static_cast<double>(m[i]));
  }
  return sum > kValueEpsilon;
}

auto BuildFallbackXyzToCamera(const OperatorParams& params, cv::Matx33d& out) -> bool {
  if (!HasValidCamXyz(params.raw_cam_xyz_)) {
    return false;
  }

  const double g = std::max(static_cast<double>(params.raw_pre_mul_[1]), kValueEpsilon);
  const cv::Matx33d pre =
      cv::Matx33d::diag(cv::Vec3d(params.raw_pre_mul_[0] / g, 1.0, params.raw_pre_mul_[2] / g));
  const cv::Matx33d cam_xyz(params.raw_cam_xyz_[0], params.raw_cam_xyz_[1], params.raw_cam_xyz_[2],
                            params.raw_cam_xyz_[3], params.raw_cam_xyz_[4], params.raw_cam_xyz_[5],
                            params.raw_cam_xyz_[6], params.raw_cam_xyz_[7], params.raw_cam_xyz_[8]);
  out = pre * cam_xyz;
  return IsFiniteMatrix(out);
}

auto SanitizeEndpointCct(double cct, double fallback) -> double {
  return (std::isfinite(cct) && cct > kValueEpsilon) ? cct : fallback;
}

auto HasDualIlluminantCalibration(const OperatorParams& params) -> bool {
  if (!params.raw_calibration_illuminants_valid_) {
    return false;
  }

  const double cct1 = params.raw_color_matrix_1_cct_;
  const double cct2 = params.raw_color_matrix_2_cct_;
  return std::isfinite(cct1) && std::isfinite(cct2) && cct1 > kValueEpsilon &&
         cct2 > kValueEpsilon && std::abs(cct1 - cct2) > kValueEpsilon;
}

auto InterpolateColorMatrix(const cv::Matx33d& cm1, const cv::Matx33d& cm2, double cct,
                            double endpoint_cct_1, double endpoint_cct_2)
    -> cv::Matx33d {
  const double cct1 = SanitizeEndpointCct(endpoint_cct_1, kCalibrationLowCCT);
  const double cct2 = SanitizeEndpointCct(endpoint_cct_2, kCalibrationHighCCT);
  const double low_cct  = std::min(cct1, cct2);
  const double high_cct = std::max(cct1, cct2);
  const bool   cm1_is_low = cct1 <= cct2;
  const cv::Matx33d& low_matrix  = cm1_is_low ? cm1 : cm2;
  const cv::Matx33d& high_matrix = cm1_is_low ? cm2 : cm1;

  if (!std::isfinite(cct)) {
    return high_matrix;
  }

  if (cct <= low_cct) {
    return low_matrix;
  }
  if (cct >= high_cct) {
    return high_matrix;
  }

  const double inv_t  = 1.0 / cct;
  const double inv_t1 = 1.0 / low_cct;
  const double inv_t2 = 1.0 / high_cct;
  const double denom  = inv_t2 - inv_t1;
  if (std::abs(denom) <= kValueEpsilon) {
    return high_matrix;
  }

  const double w = std::clamp((inv_t - inv_t1) / denom, 0.0, 1.0);
  return low_matrix * (1.0 - w) + high_matrix * w;
}

auto Invert3x3(const cv::Matx33d& m, cv::Matx33d& out) -> bool {
  const double det = cv::determinant(m);
  if (!std::isfinite(det) || std::abs(det) < kDeterminantEpsilon) {
    return false;
  }
  out = m.inv();
  return IsFiniteMatrix(out);
}

auto XYZToXY(const cv::Vec3d& xyz, cv::Vec2d& out_xy) -> bool {
  const double sum = xyz[0] + xyz[1] + xyz[2];
  if (!std::isfinite(sum) || std::abs(sum) <= kValueEpsilon) {
    return false;
  }
  const double x = xyz[0] / sum;
  const double y = xyz[1] / sum;
  if (!std::isfinite(x) || !std::isfinite(y) || y <= 0.0 || x <= 0.0 || (x + y) >= 1.0) {
    return false;
  }
  out_xy = cv::Vec2d(x, y);
  return true;
}

auto XYToXYZ(const cv::Vec2d& xy) -> cv::Vec3d {
  const double y = std::max(xy[1], kValueEpsilon);
  const double X = xy[0] / y;
  const double Y = 1.0;
  const double Z = (1.0 - xy[0] - xy[1]) / y;
  return cv::Vec3d(X, Y, Z);
}

auto XYToUV(const cv::Vec2d& xy) -> cv::Vec2d {
  const double den = (-xy[0] + 6.0 * xy[1] + 1.5);
  if (std::abs(den) <= kValueEpsilon) {
    return cv::Vec2d(0.0, 0.0);
  }
  return cv::Vec2d(2.0 * xy[0] / den, 3.0 * xy[1] / den);
}

auto UVToXY(const cv::Vec2d& uv) -> cv::Vec2d {
  const double den = (uv[0] - 4.0 * uv[1] + 2.0);
  if (std::abs(den) <= kValueEpsilon) {
    return cv::Vec2d(kD50X, kD50Y);
  }
  return cv::Vec2d(1.5 * uv[0] / den, uv[1] / den);
}

struct PlanckianLocusPoint {
  cv::Vec2d uv_;     // point on the Planckian locus
  cv::Vec2d perp_;   // normalized isotherm direction at this CCT
  bool      valid_;  // false when the perpendicular direction degenerates
};

struct OhnoSolution {
  bool   valid_ = false;
  double cct_   = 0.0;
  double duv_   = 0.0;
};

using PlanckianTable = std::span<const planckian_locus::PlanckianLocusEntry>;

auto OhnoPlanckianTable() -> PlanckianTable {
  return PlanckianTable(planckian_locus::kUvTable.data(), planckian_locus::kUvTable.size());
}

auto EntryUV(const planckian_locus::PlanckianLocusEntry& entry) -> cv::Vec2d {
  return cv::Vec2d(entry.u_, entry.v_);
}

auto PlanckianUVAtCCT(double cct) -> cv::Vec2d {
  const auto table = OhnoPlanckianTable();
  const double safe_cct =
      ClampFinite(cct, planckian_locus::kMinCct, planckian_locus::kMaxCct);

  if (safe_cct <= table.front().cct_) {
    return EntryUV(table.front());
  }
  if (safe_cct >= table.back().cct_) {
    return EntryUV(table.back());
  }

  const auto upper = std::lower_bound(table.begin(), table.end(), safe_cct,
                                      [](const planckian_locus::PlanckianLocusEntry& entry,
                                         double value) { return entry.cct_ < value; });
  const auto lower = upper - 1;
  const double denom = upper->cct_ - lower->cct_;
  if (std::abs(denom) <= kValueEpsilon) {
    return EntryUV(*upper);
  }

  const double t = std::clamp((safe_cct - lower->cct_) / denom, 0.0, 1.0);
  return EntryUV(*lower) * (1.0 - t) + EntryUV(*upper) * t;
}

auto PlanckianLocusAtCCT(double cct) -> PlanckianLocusPoint {
  const double safe_cct = ClampFinite(cct, kCustomCCTMin, kCustomCCTMax);
  const double delta_k =
      std::clamp(safe_cct * 0.001, 0.5, std::max(0.5, 0.25 * (kCustomCCTMax - kCustomCCTMin)));
  const double low_cct  = std::max(kCustomCCTMin, safe_cct - delta_k);
  const double high_cct = std::min(kCustomCCTMax, safe_cct + delta_k);

  PlanckianLocusPoint pt;
  pt.uv_ = PlanckianUVAtCCT(safe_cct);
  if ((high_cct - low_cct) <= kValueEpsilon) {
    pt.perp_  = cv::Vec2d(0.0, 0.0);
    pt.valid_ = false;
    return pt;
  }

  const cv::Vec2d tangent = (PlanckianUVAtCCT(high_cct) - PlanckianUVAtCCT(low_cct)) /
                            (high_cct - low_cct);
  const double tangent_len = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1]);

  if (tangent_len <= kValueEpsilon) {
    pt.perp_  = cv::Vec2d(0.0, 0.0);
    pt.valid_ = false;
  } else {
    pt.perp_  = cv::Vec2d(-tangent[1] / tangent_len, tangent[0] / tangent_len);
    pt.valid_ = true;
  }
  return pt;
}

auto Distance(const cv::Vec2d& a, const cv::Vec2d& b) -> double {
  const double du = a[0] - b[0];
  const double dv = a[1] - b[1];
  return std::sqrt(du * du + dv * dv);
}

auto SignFromDeltaV(double delta_v) -> double { return (delta_v < 0.0) ? -1.0 : 1.0; }

auto FindClosestOhnoSample(const cv::Vec2d& uv, PlanckianTable table) -> size_t {
  size_t best_index       = 0;
  double best_distance_sq = std::numeric_limits<double>::max();

  for (size_t i = 0; i < table.size(); ++i) {
    const double du          = uv[0] - table[i].u_;
    const double dv          = uv[1] - table[i].v_;
    const double distance_sq = du * du + dv * dv;
    if (distance_sq < best_distance_sq) {
      best_distance_sq = distance_sq;
      best_index       = i;
    }
  }

  if (best_index == 0) {
    return 1;
  }
  if (best_index + 1 >= table.size()) {
    return table.size() - 2;
  }
  return best_index;
}

auto SolveOhnoTriangular(const cv::Vec2d& uv, PlanckianTable table, size_t m) -> OhnoSolution {
  const auto& p0 = table[m - 1];
  const auto& p2 = table[m + 1];
  const cv::Vec2d p0_uv = EntryUV(p0);
  const cv::Vec2d p2_uv = EntryUV(p2);

  const double d0 = Distance(uv, p0_uv);
  const double d2 = Distance(uv, p2_uv);

  const cv::Vec2d chord = p2_uv - p0_uv;
  const double    l     = std::sqrt(chord[0] * chord[0] + chord[1] * chord[1]);
  if (l <= kValueEpsilon) {
    return {};
  }

  const double x     = std::clamp((d0 * d0 - d2 * d2 + l * l) / (2.0 * l), 0.0, l);
  const double alpha = x / l;
  const double cct   = p0.cct_ + (p2.cct_ - p0.cct_) * alpha;
  const double v_tx  = p0.v_ + (p2.v_ - p0.v_) * alpha;
  const double duv   = std::sqrt(std::max(0.0, d0 * d0 - x * x)) * SignFromDeltaV(uv[1] - v_tx);

  return OhnoSolution{std::isfinite(cct) && std::isfinite(duv), cct, duv};
}

auto SolveOhnoParabolic(const cv::Vec2d& uv, PlanckianTable table, size_t m) -> OhnoSolution {
  const auto& p0 = table[m - 1];
  const auto& p1 = table[m];
  const auto& p2 = table[m + 1];

  const double t0 = p0.cct_ * 0.001;
  const double t1 = p1.cct_ * 0.001;
  const double t2 = p2.cct_ * 0.001;
  const double d0 = Distance(uv, EntryUV(p0));
  const double d1 = Distance(uv, EntryUV(p1));
  const double d2 = Distance(uv, EntryUV(p2));

  const double denom0 = (t0 - t1) * (t0 - t2);
  const double denom1 = (t1 - t0) * (t1 - t2);
  const double denom2 = (t2 - t0) * (t2 - t1);
  if (std::abs(denom0) <= kValueEpsilon || std::abs(denom1) <= kValueEpsilon ||
      std::abs(denom2) <= kValueEpsilon) {
    return {};
  }

  const double a = d0 / denom0 + d1 / denom1 + d2 / denom2;
  const double b = -d0 * (t1 + t2) / denom0 - d1 * (t0 + t2) / denom1 -
                   d2 * (t0 + t1) / denom2;
  const double c = d0 * t1 * t2 / denom0 + d1 * t0 * t2 / denom1 +
                   d2 * t0 * t1 / denom2;
  if (!std::isfinite(a) || std::abs(a) <= kValueEpsilon) {
    return {};
  }

  const double tx       = std::clamp(-b / (2.0 * a), t0, t2);
  const double cct      = tx * 1000.0;
  const double distance = std::max(0.0, a * tx * tx + b * tx + c);
  const double v_tx     = PlanckianUVAtCCT(cct)[1];
  const double duv      = distance * SignFromDeltaV(uv[1] - v_tx);

  return OhnoSolution{std::isfinite(cct) && std::isfinite(duv), cct, duv};
}

auto OhnoTemperatureDuv(const cv::Vec2d& uv, double& out_cct, double& out_duv) -> bool {
  if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) {
    return false;
  }

  const auto table = OhnoPlanckianTable();
  if (table.size() < 3) {
    return false;
  }

  const size_t       m           = FindClosestOhnoSample(uv, table);
  const OhnoSolution triangular  = SolveOhnoTriangular(uv, table, m);
  const OhnoSolution parabolic   = SolveOhnoParabolic(uv, table, m);
  const OhnoSolution& selected =
      (triangular.valid_ && (std::abs(triangular.duv_) < kOhnoNearLocusDuv || !parabolic.valid_))
          ? triangular
          : parabolic;

  if (!selected.valid_) {
    return false;
  }

  out_cct = ClampFinite(selected.cct_, kCustomCCTMin, kCustomCCTMax);
  out_duv = selected.duv_;
  return std::isfinite(out_cct) && std::isfinite(out_duv);
}

auto UVToTemperatureTint(const cv::Vec2d& uv, double& out_cct, double& out_tint) -> bool {
  double duv = 0.0;
  if (!OhnoTemperatureDuv(uv, out_cct, duv)) {
    return false;
  }

  const auto locus = PlanckianLocusAtCCT(out_cct);
  if (!locus.valid_) {
    return false;
  }

  const double delta_u = uv[0] - locus.uv_[0];
  const double delta_v = uv[1] - locus.uv_[1];
  out_tint = -kTintScale * (delta_u * locus.perp_[0] + delta_v * locus.perp_[1]);
  return std::isfinite(out_cct) && std::isfinite(out_tint);
}

auto TemperatureTintToUV(double cct, double tint) -> cv::Vec2d {
  const auto locus = PlanckianLocusAtCCT(cct);
  if (!locus.valid_) {
    return locus.uv_;
  }
  const double duv = ClampFinite(tint, kCustomTintMin, kCustomTintMax) / kTintScale;
  return cv::Vec2d(locus.uv_[0] - locus.perp_[0] * duv,
                   locus.uv_[1] - locus.perp_[1] * duv);
}

auto BuildBradfordCAT(const cv::Vec2d& src_xy, const cv::Vec2d& dst_xy) -> cv::Matx33d {
  static const cv::Matx33d kBradford(0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389,
                                     -0.0685, 1.0296);
  static const cv::Matx33d kBradfordInv(0.9869929, -0.1470543, 0.1599627, 0.4323053, 0.5183603,
                                        0.0492912, -0.0085287, 0.0400428, 0.9684867);

  const cv::Vec3d src_xyz = XYToXYZ(src_xy);
  const cv::Vec3d dst_xyz = XYToXYZ(dst_xy);

  const cv::Vec3d src_lms = kBradford * src_xyz;
  const cv::Vec3d dst_lms = kBradford * dst_xyz;

  const cv::Matx33d diag = cv::Matx33d::diag(cv::Vec3d(
      dst_lms[0] / std::max(src_lms[0], kValueEpsilon), dst_lms[1] / std::max(src_lms[1], kValueEpsilon),
      dst_lms[2] / std::max(src_lms[2], kValueEpsilon)));
  return kBradfordInv * diag * kBradford;
}

void StoreMatrix(const cv::Matx33d& src, float dst[9]) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      dst[r * 3 + c] = static_cast<float>(src(r, c));
    }
  }
}

auto ResolveColorMatrixEndpoints(const OperatorParams& params, cv::Matx33d& cm1, cv::Matx33d& cm2,
                                 double& endpoint_cct_1, double& endpoint_cct_2) -> bool {
  endpoint_cct_1 = kCalibrationLowCCT;
  endpoint_cct_2 = kCalibrationHighCCT;

  // Use pre-resolved Adobe DNG colour matrices stored in OperatorParams.
  // These are looked up once at import time by MetadataExtractor.
  if (params.raw_color_matrices_valid_) {
    cm1 = MatrixFromArray9(params.raw_color_matrix_1_);
    cm2 = MatrixFromArray9(params.raw_color_matrix_2_);
    if (HasDualIlluminantCalibration(params)) {
      endpoint_cct_1 = params.raw_color_matrix_1_cct_;
      endpoint_cct_2 = params.raw_color_matrix_2_cct_;
    } else {
      // Match Adobe DNG SDK behavior: if either calibration illuminant is
      // missing/invalid, treat the profile as a single calibration instead of
      // interpolating Matrix1/2 across a fabricated 2856K/6504K range.
      cm2            = cm1;
      endpoint_cct_1 = 5000.0;
      endpoint_cct_2 = 5000.0;
    }
    return IsFiniteMatrix(cm1) && IsFiniteMatrix(cm2);
  }

  // Fallback: derive a single matrix from libraw cam_xyz / pre_mul.
  cv::Matx33d fallback;
  if (!BuildFallbackXyzToCamera(params, fallback)) {
    return false;
  }
  cm1 = fallback;
  cm2 = fallback;
  return true;
}

auto ResolveForwardMatrixEndpoints(const OperatorParams& params, cv::Matx33d& fm1,
                                   cv::Matx33d& fm2) -> bool {
  if (!params.raw_forward_matrices_valid_) {
    return false;
  }

  fm1 = MatrixFromArray9(params.raw_forward_matrix_1_);
  fm2 = MatrixFromArray9(params.raw_forward_matrix_2_);
  if (!HasDualIlluminantCalibration(params)) {
    fm2 = fm1;
  }
  return IsFiniteMatrix(fm1) && IsFiniteMatrix(fm2);
}

auto ResolveAsShotCameraNeutral(const OperatorParams& params, cv::Vec3d& out_neutral) -> bool {
  if (params.raw_as_shot_neutral_valid_) {
    const cv::Vec3d neutral(params.raw_as_shot_neutral_[0], params.raw_as_shot_neutral_[1],
                            params.raw_as_shot_neutral_[2]);
    if (std::all_of(std::begin(neutral.val), std::end(neutral.val), [](double value) {
          return std::isfinite(value) && value > kValueEpsilon;
        })) {
      out_neutral = neutral;
      return true;
    }
  }

  const double r = std::max(static_cast<double>(params.raw_cam_mul_[0]), kValueEpsilon);
  const double g = std::max(static_cast<double>(params.raw_cam_mul_[1]), kValueEpsilon);
  const double b = std::max(static_cast<double>(params.raw_cam_mul_[2]), kValueEpsilon);
  if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
    return false;
  }

  out_neutral = cv::Vec3d(g / r, 1.0, g / b);
  return std::all_of(std::begin(out_neutral.val), std::end(out_neutral.val), [](double value) {
    return std::isfinite(value) && value > kValueEpsilon;
  });
}

auto TintAtCctForXY(double cct, const cv::Vec2d& xy, double& out_tint) -> bool {
  const auto locus = PlanckianLocusAtCCT(cct);
  if (!locus.valid_) {
    return false;
  }

  const cv::Vec2d uv = XYToUV(xy);
  const double delta_u = uv[0] - locus.uv_[0];
  const double delta_v = uv[1] - locus.uv_[1];
  out_tint = -kTintScale * (delta_u * locus.perp_[0] + delta_v * locus.perp_[1]);
  return std::isfinite(out_tint);
}

struct AsShotCctEvaluation {
  bool      valid_         = false;
  double    candidate_cct_ = 0.0;
  double    derived_cct_   = 0.0;
  double    residual_      = 0.0;
  double    tint_          = 0.0;
  cv::Vec2d xy_            = cv::Vec2d(kD50X, kD50Y);
};

auto EvaluateAsShotAtCct(const cv::Vec3d& camera_neutral, const cv::Matx33d& cm1,
                         const cv::Matx33d& cm2, double endpoint_cct_1,
                         double endpoint_cct_2, double candidate_cct)
    -> AsShotCctEvaluation {
  AsShotCctEvaluation eval;
  eval.candidate_cct_ = ClampFinite(candidate_cct, kCustomCCTMin, kCustomCCTMax);

  const cv::Matx33d xyz_to_camera =
      InterpolateColorMatrix(cm1, cm2, eval.candidate_cct_, endpoint_cct_1, endpoint_cct_2);
  cv::Matx33d camera_to_xyz;
  if (!Invert3x3(xyz_to_camera, camera_to_xyz)) {
    return eval;
  }

  const cv::Vec3d white_xyz = camera_to_xyz * camera_neutral;
  if (!XYZToXY(white_xyz, eval.xy_)) {
    return eval;
  }

  double derived_tint = 0.0;
  if (!UVToTemperatureTint(XYToUV(eval.xy_), eval.derived_cct_, derived_tint)) {
    return eval;
  }
  if (!TintAtCctForXY(eval.candidate_cct_, eval.xy_, eval.tint_)) {
    return eval;
  }

  eval.residual_ = eval.derived_cct_ - eval.candidate_cct_;
  eval.valid_    = std::isfinite(eval.residual_) && std::isfinite(eval.tint_);
  return eval;
}

auto IsBetterAsShotEvaluation(const AsShotCctEvaluation& candidate,
                              const AsShotCctEvaluation& incumbent) -> bool {
  if (!candidate.valid_) {
    return false;
  }
  if (!incumbent.valid_) {
    return true;
  }
  return std::abs(candidate.residual_) < std::abs(incumbent.residual_);
}

auto HasOppositeSigns(double a, double b) -> bool {
  return (a <= 0.0 && b >= 0.0) || (a >= 0.0 && b <= 0.0);
}

auto RefineAsShotCctRoot(const cv::Vec3d& camera_neutral, const cv::Matx33d& cm1,
                         const cv::Matx33d& cm2, double endpoint_cct_1,
                         double endpoint_cct_2, AsShotCctEvaluation low,
                         AsShotCctEvaluation high) -> AsShotCctEvaluation {
  AsShotCctEvaluation best = IsBetterAsShotEvaluation(low, high) ? low : high;

  for (int i = 0; i < kAsShotSolveMaxIter; ++i) {
    const double mid_cct = 0.5 * (low.candidate_cct_ + high.candidate_cct_);
    auto mid = EvaluateAsShotAtCct(camera_neutral, cm1, cm2, endpoint_cct_1, endpoint_cct_2,
                                   mid_cct);
    if (!mid.valid_) {
      break;
    }
    if (IsBetterAsShotEvaluation(mid, best)) {
      best = mid;
    }
    if (std::abs(mid.residual_) <= kAsShotSolveEpsilon) {
      return mid;
    }
    if (HasOppositeSigns(low.residual_, mid.residual_)) {
      high = mid;
    } else {
      low = mid;
    }
  }

  return best;
}

auto SolveAsShotWhiteXY(const OperatorParams& params, const cv::Matx33d& cm1, const cv::Matx33d& cm2,
                        double endpoint_cct_1, double endpoint_cct_2, cv::Vec2d& out_xy,
                        double& out_cct, double& out_tint) -> bool {
  cv::Vec3d camera_neutral;
  if (!ResolveAsShotCameraNeutral(params, camera_neutral)) {
    return false;
  }

  AsShotCctEvaluation best;
  AsShotCctEvaluation previous;
  constexpr int       kScanCount = 96;
  const double        min_mired  = 1.0e6 / kCustomCCTMax;
  const double        max_mired  = 1.0e6 / kCustomCCTMin;

  for (int i = 0; i <= kScanCount; ++i) {
    const double t     = static_cast<double>(i) / static_cast<double>(kScanCount);
    const double mired = max_mired + (min_mired - max_mired) * t;
    const double cct   = 1.0e6 / std::max(mired, kValueEpsilon);
    auto eval = EvaluateAsShotAtCct(camera_neutral, cm1, cm2, endpoint_cct_1, endpoint_cct_2,
                                    cct);
    if (!eval.valid_) {
      continue;
    }
    if (IsBetterAsShotEvaluation(eval, best)) {
      best = eval;
    }
    if (previous.valid_ && HasOppositeSigns(previous.residual_, eval.residual_)) {
      auto refined = RefineAsShotCctRoot(camera_neutral, cm1, cm2, endpoint_cct_1,
                                         endpoint_cct_2, previous, eval);
      if (IsBetterAsShotEvaluation(refined, best)) {
        best = refined;
      }
    }
    previous = eval;
  }

  if (!best.valid_) {
    return false;
  }

  out_xy   = best.xy_;
  out_cct  = best.candidate_cct_;
  out_tint = best.tint_;
  return true;
}

auto BuildCameraToXyzD50FromForwardMatrix(const cv::Matx33d& cm1, const cv::Matx33d& cm2,
                                          const cv::Matx33d& fm1, const cv::Matx33d& fm2,
                                          double cct, double endpoint_cct_1,
                                          double endpoint_cct_2, const cv::Vec2d& white_xy,
                                          cv::Matx33d& out_camera_to_xyz_d50) -> bool {
  const cv::Matx33d xyz_to_camera =
      InterpolateColorMatrix(cm1, cm2, cct, endpoint_cct_1, endpoint_cct_2);
  const cv::Matx33d forward_matrix =
      InterpolateColorMatrix(fm1, fm2, cct, endpoint_cct_1, endpoint_cct_2);

  const cv::Vec3d reference_neutral = xyz_to_camera * XYToXYZ(white_xy);
  if (!std::all_of(std::begin(reference_neutral.val), std::end(reference_neutral.val),
                   [](double value) { return std::isfinite(value) && value > kValueEpsilon; })) {
    return false;
  }

  const cv::Matx33d reference_scale = cv::Matx33d::diag(
      cv::Vec3d(1.0 / reference_neutral[0], 1.0 / reference_neutral[1], 1.0 / reference_neutral[2]));
  out_camera_to_xyz_d50 = forward_matrix * reference_scale;
  return IsFiniteMatrix(out_camera_to_xyz_d50);
}
}  // namespace

ColorTempOp::ColorTempOp(const nlohmann::json& params) { SetParams(params); }

auto ColorTempOp::ParseMode(const std::string& mode) -> ColorTempMode {
  if (mode == "custom") {
    return ColorTempMode::CUSTOM;
  }
  if (mode == "as-shot" || mode == "as_shot") {
    return ColorTempMode::AS_SHOT;
  }
  return ColorTempMode::AS_SHOT;
}

auto ColorTempOp::ModeToString(ColorTempMode mode) -> std::string {
  switch (mode) {
    case ColorTempMode::CUSTOM:
      return "custom";
    case ColorTempMode::AS_SHOT:
    default:
      return "as_shot";
  }
}

void ColorTempOp::Apply(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error(
      "ColorTempOp: descriptor-only operator. Runtime matrices are resolved into global params.");
}

void ColorTempOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error(
      "ColorTempOp: descriptor-only operator. Runtime matrices are resolved into global params.");
}

auto ColorTempOp::GetParams() const -> nlohmann::json {
  const float effective_cct =
      (mode_ == ColorTempMode::AS_SHOT) ? resolved_cct_ : custom_cct_;
  const float effective_tint =
      (mode_ == ColorTempMode::AS_SHOT) ? resolved_tint_ : custom_tint_;

  nlohmann::json out;
  out[std::string(script_name_)] = {{"mode", ModeToString(mode_)},
                                    {"cct", effective_cct},
                                    {"tint", effective_tint},
                                    {"resolved_cct", resolved_cct_},
                                    {"resolved_tint", resolved_tint_}};
  return out;
}

void ColorTempOp::SetParams(const nlohmann::json& params) {
  if (!params.contains(script_name_)) {
    return;
  }

  const auto& j = params[script_name_];
  if (j.contains("mode") && j["mode"].is_string()) {
    mode_ = ParseMode(j["mode"].get<std::string>());
  }
  if (j.contains("cct")) {
    custom_cct_ =
        static_cast<float>(ClampFinite(j["cct"].get<double>(), kCustomCCTMin, kCustomCCTMax));
  }
  if (j.contains("tint")) {
    custom_tint_ =
        static_cast<float>(ClampFinite(j["tint"].get<double>(), kCustomTintMin, kCustomTintMax));
  }
  // Image-local as-shot cache: only overwrite when the payload explicitly carries
  // resolved_* keys. Missing keys must not fall back to custom_* — transfer packages
  // intentionally strip as-shot CCT/Tint, and custom→as_shot must keep the target's
  // as-shot baseline until ResolveRuntime refreshes it from RAW context.
  if (j.contains("resolved_cct")) {
    resolved_cct_ = static_cast<float>(
        ClampFinite(j["resolved_cct"].get<double>(), kCustomCCTMin, kCustomCCTMax));
  }
  if (j.contains("resolved_tint")) {
    resolved_tint_ = static_cast<float>(
        ClampFinite(j["resolved_tint"].get<double>(), kCustomTintMin, kCustomTintMax));
  }
}

namespace {

auto ColorTempInner(const nlohmann::json& params) -> nlohmann::json {
  if (params.contains("color_temp") && params["color_temp"].is_object()) {
    return params["color_temp"];
  }
  return nlohmann::json::object();
}

auto ColorTempModeFromParams(const nlohmann::json& params) -> ColorTempMode {
  const auto inner = ColorTempInner(params);
  if (!inner.contains("mode") || !inner["mode"].is_string()) {
    return ColorTempMode::AS_SHOT;
  }
  const auto mode = inner["mode"].get<std::string>();
  if (mode == "custom") {
    return ColorTempMode::CUSTOM;
  }
  return ColorTempMode::AS_SHOT;
}

auto ColorTempAsShotBaseline(const nlohmann::json& params, double& out_cct, double& out_tint)
    -> void {
  const auto inner = ColorTempInner(params);
  // Prefer explicit resolved_* (as-shot cache). When the op was already as_shot,
  // GetParams mirrors those into cct/tint as well.
  if (inner.contains("resolved_cct") && inner["resolved_cct"].is_number()) {
    out_cct = inner["resolved_cct"].get<double>();
  } else if (inner.contains("cct") && inner["cct"].is_number() &&
             ColorTempModeFromParams(params) == ColorTempMode::AS_SHOT) {
    out_cct = inner["cct"].get<double>();
  } else {
    out_cct = 6500.0;
  }
  if (inner.contains("resolved_tint") && inner["resolved_tint"].is_number()) {
    out_tint = inner["resolved_tint"].get<double>();
  } else if (inner.contains("tint") && inner["tint"].is_number() &&
             ColorTempModeFromParams(params) == ColorTempMode::AS_SHOT) {
    out_tint = inner["tint"].get<double>();
  } else {
    out_tint = 0.0;
  }
}

}  // namespace

auto ColorTempOp::DetectMergeConflict(const nlohmann::json& current,
                                      const nlohmann::json& incoming) const -> bool {
  const auto current_mode  = ColorTempModeFromParams(current);
  const auto incoming_mode = ColorTempModeFromParams(incoming);
  // as_shot is portable intent only; image-local CCT/Tint must not force a conflict.
  if (current_mode == ColorTempMode::AS_SHOT && incoming_mode == ColorTempMode::AS_SHOT) {
    return false;
  }
  if (current_mode != incoming_mode) {
    return true;
  }
  // Both custom: compare user-authored CCT/Tint only.
  const auto cur = ColorTempInner(current);
  const auto inc = ColorTempInner(incoming);
  const double cur_cct  = cur.value("cct", 6500.0);
  const double cur_tint = cur.value("tint", 0.0);
  const double inc_cct  = inc.value("cct", 6500.0);
  const double inc_tint = inc.value("tint", 0.0);
  constexpr double kCctEps  = 0.5;
  constexpr double kTintEps = 0.05;
  return std::abs(cur_cct - inc_cct) > kCctEps || std::abs(cur_tint - inc_tint) > kTintEps;
}

auto ColorTempOp::MergeParams(const nlohmann::json& current, const nlohmann::json& incoming,
                              OperatorMergeChoice choice) const -> nlohmann::json {
  if (choice == OperatorMergeChoice::kKeepCurrent) {
    return current;
  }

  const auto incoming_mode = ColorTempModeFromParams(incoming);
  nlohmann::json result =
      current.is_object() ? current : nlohmann::json{{std::string(script_name_), nlohmann::json::object()}};
  if (!result.contains(std::string(script_name_)) || !result[std::string(script_name_)].is_object()) {
    result[std::string(script_name_)] = nlohmann::json::object();
  }
  auto& out = result[std::string(script_name_)];

  if (incoming_mode == ColorTempMode::AS_SHOT) {
    double baseline_cct  = 6500.0;
    double baseline_tint = 0.0;
    ColorTempAsShotBaseline(current, baseline_cct, baseline_tint);
    out["mode"]          = "as_shot";
    out["cct"]           = baseline_cct;
    out["tint"]          = baseline_tint;
    out["resolved_cct"]  = baseline_cct;
    out["resolved_tint"] = baseline_tint;
    return result;
  }

  // Take custom values from incoming; keep current's as-shot baseline in resolved_*.
  const auto inc = ColorTempInner(incoming);
  double baseline_cct  = 6500.0;
  double baseline_tint = 0.0;
  ColorTempAsShotBaseline(current, baseline_cct, baseline_tint);
  out["mode"] = "custom";
  if (inc.contains("cct") && inc["cct"].is_number()) {
    out["cct"] = inc["cct"];
  }
  if (inc.contains("tint") && inc["tint"].is_number()) {
    out["tint"] = inc["tint"];
  }
  out["resolved_cct"]  = baseline_cct;
  out["resolved_tint"] = baseline_tint;
  return result;
}

void ColorTempOp::SetGlobalParams(OperatorParams& params) const {
  params.color_temp_mode_         = mode_;
  params.color_temp_custom_cct_   = custom_cct_;
  params.color_temp_custom_tint_  = custom_tint_;
  params.color_temp_resolved_cct_ = resolved_cct_;
  params.color_temp_resolved_tint_ = resolved_tint_;
  params.color_temp_runtime_dirty_ = true;

  // Eagerly resolve camera→XYZ/AP1 matrices now instead of deferring to pipeline Apply.
  ResolveRuntime(params);
}

void ColorTempOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.color_temp_enabled_      = enable;
  params.color_temp_runtime_dirty_ = true;
}

void ColorTempOp::ResolveRuntime(OperatorParams& params) const {
  params.color_temp_mode_        = mode_;
  params.color_temp_custom_cct_  = custom_cct_;
  params.color_temp_custom_tint_ = custom_tint_;

  const std::uint64_t runtime_cache_key =
      BuildRuntimeCacheKey(params, mode_, custom_cct_, custom_tint_);
  if (params.color_temp_cache_key_valid_ && params.color_temp_cache_key_ == runtime_cache_key) {
    resolved_cct_                 = params.color_temp_resolved_cct_;
    resolved_tint_                = params.color_temp_resolved_tint_;
    params.color_temp_runtime_dirty_ = false;
    return;
  }
  params.color_temp_cache_key_       = runtime_cache_key;
  params.color_temp_cache_key_valid_ = true;

  if (!params.color_temp_enabled_) {
    params.color_temp_matrices_valid_ = false;
    params.color_temp_runtime_dirty_  = false;
    return;
  }
  if (!params.raw_runtime_valid_) {
    params.color_temp_matrices_valid_ = false;
    params.color_temp_runtime_dirty_  = false;
    return;
  }

  cv::Matx33d cm1;
  cv::Matx33d cm2;
  cv::Matx33d fm1;
  cv::Matx33d fm2;
  double     endpoint_cct_1 = kCalibrationLowCCT;
  double     endpoint_cct_2 = kCalibrationHighCCT;
  if (!ResolveColorMatrixEndpoints(params, cm1, cm2, endpoint_cct_1, endpoint_cct_2)) {
    params.color_temp_matrices_valid_ = false;
    params.color_temp_runtime_dirty_  = false;
    return;
  }
  const bool has_forward_matrices = ResolveForwardMatrixEndpoints(params, fm1, fm2);

  cv::Vec2d selected_xy(kD50X, kD50Y);
  double    selected_cct  = custom_cct_;
  double    selected_tint = custom_tint_;

  if (mode_ == ColorTempMode::AS_SHOT) {
    if (!SolveAsShotWhiteXY(params, cm1, cm2, endpoint_cct_1, endpoint_cct_2, selected_xy,
                            selected_cct, selected_tint)) {
      params.color_temp_matrices_valid_ = false;
      params.color_temp_runtime_dirty_  = false;
      return;
    }
  } else {
    selected_cct  = ClampFinite(custom_cct_, kCustomCCTMin, kCustomCCTMax);
    selected_tint = ClampFinite(custom_tint_, kCustomTintMin, kCustomTintMax);
    selected_xy   = UVToXY(TemperatureTintToUV(selected_cct, selected_tint));
  }

  const cv::Matx33d xyz_to_camera =
      InterpolateColorMatrix(cm1, cm2, selected_cct, endpoint_cct_1, endpoint_cct_2);
  cv::Matx33d       camera_to_xyz;
  if (!Invert3x3(xyz_to_camera, camera_to_xyz)) {
    std::cout << "ColorTempOp: Failed to invert XYZ to camera matrix.\n";
    params.color_temp_matrices_valid_ = false;
    params.color_temp_runtime_dirty_  = false;
    return;
  }

  cv::Matx33d camera_to_xyz_d50;
  if (has_forward_matrices) {
    if (!BuildCameraToXyzD50FromForwardMatrix(cm1, cm2, fm1, fm2, selected_cct, endpoint_cct_1,
                                              endpoint_cct_2, selected_xy, camera_to_xyz_d50)) {
      params.color_temp_matrices_valid_ = false;
      params.color_temp_runtime_dirty_  = false;
      return;
    }
  } else {
    const cv::Matx33d cat_src_to_d50 = BuildBradfordCAT(selected_xy, cv::Vec2d(kD50X, kD50Y));
    camera_to_xyz_d50                = cat_src_to_d50 * camera_to_xyz;
  }

  const cv::Matx33d cat_d50_to_d60 = BuildBradfordCAT(cv::Vec2d(kD50X, kD50Y), cv::Vec2d(kD60X, kD60Y));
  const cv::Matx33d xyz_d50_to_ap1 = kXyzD60ToAp1 * cat_d50_to_d60;
  const cv::Matx33d camera_to_ap1  = xyz_d50_to_ap1 * camera_to_xyz_d50;

  StoreMatrix(camera_to_xyz, params.color_temp_cam_to_xyz_);
  StoreMatrix(camera_to_xyz_d50, params.color_temp_cam_to_xyz_d50_);
  StoreMatrix(xyz_d50_to_ap1, params.color_temp_xyz_d50_to_ap1_);
  StoreMatrix(camera_to_ap1, params.color_temp_cam_to_ap1_);

  resolved_cct_ = static_cast<float>(ClampFinite(selected_cct, kCustomCCTMin, kCustomCCTMax));
  resolved_tint_ = static_cast<float>(ClampFinite(selected_tint, kCustomTintMin, kCustomTintMax));

  // Reflect runtime-selected values back into effective exported params.
  params.color_temp_custom_cct_         = resolved_cct_;
  params.color_temp_custom_tint_        = resolved_tint_;
  params.color_temp_resolved_cct_       = resolved_cct_;
  params.color_temp_resolved_tint_      = resolved_tint_;
  params.color_temp_resolved_xy_[0]     = static_cast<float>(selected_xy[0]);
  params.color_temp_resolved_xy_[1]     = static_cast<float>(selected_xy[1]);
  params.color_temp_runtime_dirty_      = false;
  params.color_temp_matrices_valid_     = true;
}
}  // namespace alcedo
