//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/develop_color_transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <opencv2/core.hpp>
#include <span>

#include "edit/operators/basic/planckian_locus_table.hpp"
#include "image/dng_camera_matrix.hpp"

namespace alcedo {
namespace {

constexpr double kCalibrationLowCCT  = 2856.0;
constexpr double kCalibrationHighCCT = 6504.0;
constexpr double kCustomCCTMin       = 2000.0;
constexpr double kCustomCCTMax       = 15000.0;
constexpr double kCustomTintMin      = -150.0;
constexpr double kCustomTintMax      = 150.0;
constexpr double kTintScale          = 3000.0;
constexpr double kDeterminantEpsilon = 1e-10;
constexpr double kValueEpsilon       = 1e-10;
constexpr double kAsShotSolveEpsilon = 1e-8;
constexpr int    kAsShotSolveMaxIter = 16;
constexpr double kOhnoNearLocusDuv   = 0.002;
constexpr double kD50X               = 0.34567;
constexpr double kD50Y               = 0.35850;
constexpr double kD60X               = 0.32168;
constexpr double kD60Y               = 0.33767;
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

auto MatrixFromArray9(const std::array<double, 9>& m) -> cv::Matx33d {
  return cv::Matx33d(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
}

auto IsFiniteInvertible(const cv::Matx33d& m) -> bool {
  if (!IsFiniteMatrix(m)) {
    return false;
  }
  const double det = cv::determinant(m);
  return std::isfinite(det) && std::abs(det) >= kDeterminantEpsilon;
}

auto SanitizeEndpointCct(double cct, double fallback) -> double {
  return (std::isfinite(cct) && cct > kValueEpsilon) ? cct : fallback;
}

auto HasDualIlluminantCalibration(const DevelopCameraProfile& profile) -> bool {
  if (!profile.calibration_illuminants_valid) {
    return false;
  }
  const double cct1 = profile.color_matrix_1_cct;
  const double cct2 = profile.color_matrix_2_cct;
  return std::isfinite(cct1) && std::isfinite(cct2) && cct1 > kValueEpsilon &&
         cct2 > kValueEpsilon && std::abs(cct1 - cct2) > kValueEpsilon;
}

auto InterpolateColorMatrix(const cv::Matx33d& cm1, const cv::Matx33d& cm2, double cct,
                            double endpoint_cct_1, double endpoint_cct_2) -> cv::Matx33d {
  const double cct1       = SanitizeEndpointCct(endpoint_cct_1, kCalibrationLowCCT);
  const double cct2       = SanitizeEndpointCct(endpoint_cct_2, kCalibrationHighCCT);
  const double low_cct    = std::min(cct1, cct2);
  const double high_cct   = std::max(cct1, cct2);
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
  return cv::Vec3d(xy[0] / y, 1.0, (1.0 - xy[0] - xy[1]) / y);
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
  cv::Vec2d uv_;
  cv::Vec2d perp_;
  bool      valid_ = false;
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
  const auto   table    = OhnoPlanckianTable();
  const double safe_cct = ClampFinite(cct, planckian_locus::kMinCct, planckian_locus::kMaxCct);
  if (safe_cct <= table.front().cct_) {
    return EntryUV(table.front());
  }
  if (safe_cct >= table.back().cct_) {
    return EntryUV(table.back());
  }
  const auto upper = std::lower_bound(table.begin(), table.end(), safe_cct,
                                      [](const planckian_locus::PlanckianLocusEntry& entry,
                                         double value) { return entry.cct_ < value; });
  const auto   lower = upper - 1;
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
  const cv::Vec2d tangent =
      (PlanckianUVAtCCT(high_cct) - PlanckianUVAtCCT(low_cct)) / (high_cct - low_cct);
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
  const auto& p0    = table[m - 1];
  const auto& p2    = table[m + 1];
  const cv::Vec2d p0_uv = EntryUV(p0);
  const cv::Vec2d p2_uv = EntryUV(p2);
  const double    d0    = Distance(uv, p0_uv);
  const double    d2    = Distance(uv, p2_uv);
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
  const double b = -d0 * (t1 + t2) / denom0 - d1 * (t0 + t2) / denom1 - d2 * (t0 + t1) / denom2;
  const double c = d0 * t1 * t2 / denom0 + d1 * t0 * t2 / denom1 + d2 * t0 * t1 / denom2;
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
  const size_t       m          = FindClosestOhnoSample(uv, table);
  const OhnoSolution triangular = SolveOhnoTriangular(uv, table, m);
  const OhnoSolution parabolic  = SolveOhnoParabolic(uv, table, m);
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
  out_tint             = -kTintScale * (delta_u * locus.perp_[0] + delta_v * locus.perp_[1]);
  return std::isfinite(out_cct) && std::isfinite(out_tint);
}

auto TemperatureTintToUV(double cct, double tint) -> cv::Vec2d {
  const auto locus = PlanckianLocusAtCCT(cct);
  if (!locus.valid_) {
    return locus.uv_;
  }
  const double duv = ClampFinite(tint, kCustomTintMin, kCustomTintMax) / kTintScale;
  return cv::Vec2d(locus.uv_[0] - locus.perp_[0] * duv, locus.uv_[1] - locus.perp_[1] * duv);
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
  const cv::Matx33d diag  = cv::Matx33d::diag(cv::Vec3d(
      dst_lms[0] / std::max(src_lms[0], kValueEpsilon),
      dst_lms[1] / std::max(src_lms[1], kValueEpsilon),
      dst_lms[2] / std::max(src_lms[2], kValueEpsilon)));
  return kBradfordInv * diag * kBradford;
}

void StoreMatrix(const cv::Matx33d& src, std::array<float, 9>& dst) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      dst[static_cast<std::size_t>(r * 3 + c)] = static_cast<float>(src(r, c));
    }
  }
}

auto ResolveColorMatrixEndpoints(const DevelopCameraProfile& profile, cv::Matx33d& cm1,
                                 cv::Matx33d& cm2, double& endpoint_cct_1, double& endpoint_cct_2)
    -> ColorTransformError {
  if (!profile.color_matrices_valid) {
    return ColorTransformError::MissingCameraMatrices;
  }
  cm1 = MatrixFromArray9(profile.color_matrix_1);
  cm2 = MatrixFromArray9(profile.color_matrix_2);
  if (!IsFiniteInvertible(cm1)) {
    return ColorTransformError::SingularCameraMatrix;
  }
  if (HasDualIlluminantCalibration(profile)) {
    if (!IsFiniteInvertible(cm2)) {
      return ColorTransformError::SingularCameraMatrix;
    }
    endpoint_cct_1 = profile.color_matrix_1_cct;
    endpoint_cct_2 = profile.color_matrix_2_cct;
  } else {
    cm2            = cm1;
    endpoint_cct_1 = 5000.0;
    endpoint_cct_2 = 5000.0;
  }
  return ColorTransformError::Ok;
}

auto ResolveForwardMatrixEndpoints(const DevelopCameraProfile& profile, cv::Matx33d& fm1,
                                   cv::Matx33d& fm2) -> bool {
  if (!profile.forward_matrices_valid) {
    return false;
  }
  fm1 = MatrixFromArray9(profile.forward_matrix_1);
  fm2 = MatrixFromArray9(profile.forward_matrix_2);
  if (!HasDualIlluminantCalibration(profile)) {
    fm2 = fm1;
  }
  return IsFiniteMatrix(fm1) && IsFiniteMatrix(fm2);
}

auto ResolveAsShotCameraNeutral(const DevelopCameraProfile& profile, cv::Vec3d& out_neutral)
    -> bool {
  if (profile.as_shot_neutral_valid) {
    const cv::Vec3d neutral(profile.as_shot_neutral[0], profile.as_shot_neutral[1],
                            profile.as_shot_neutral[2]);
    if (std::all_of(std::begin(neutral.val), std::end(neutral.val), [](double value) {
          return std::isfinite(value) && value > kValueEpsilon;
        })) {
      out_neutral = neutral;
      return true;
    }
  }
  const double r = std::max(static_cast<double>(profile.cam_mul[0]), kValueEpsilon);
  const double g = std::max(static_cast<double>(profile.cam_mul[1]), kValueEpsilon);
  const double b = std::max(static_cast<double>(profile.cam_mul[2]), kValueEpsilon);
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
  const cv::Vec2d uv     = XYToUV(xy);
  const double    delta_u = uv[0] - locus.uv_[0];
  const double    delta_v = uv[1] - locus.uv_[1];
  out_tint                = -kTintScale * (delta_u * locus.perp_[0] + delta_v * locus.perp_[1]);
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
                         const cv::Matx33d& cm2, double endpoint_cct_1, double endpoint_cct_2,
                         double candidate_cct) -> AsShotCctEvaluation {
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
                         const cv::Matx33d& cm2, double endpoint_cct_1, double endpoint_cct_2,
                         AsShotCctEvaluation low, AsShotCctEvaluation high)
    -> AsShotCctEvaluation {
  AsShotCctEvaluation best = IsBetterAsShotEvaluation(low, high) ? low : high;
  for (int i = 0; i < kAsShotSolveMaxIter; ++i) {
    const double mid_cct = 0.5 * (low.candidate_cct_ + high.candidate_cct_);
    auto         mid =
        EvaluateAsShotAtCct(camera_neutral, cm1, cm2, endpoint_cct_1, endpoint_cct_2, mid_cct);
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

auto SolveAsShotWhiteXY(const DevelopCameraProfile& profile, const cv::Matx33d& cm1,
                        const cv::Matx33d& cm2, double endpoint_cct_1, double endpoint_cct_2,
                        cv::Vec2d& out_xy, double& out_cct, double& out_tint) -> bool {
  cv::Vec3d camera_neutral;
  if (!ResolveAsShotCameraNeutral(profile, camera_neutral)) {
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
    auto eval =
        EvaluateAsShotAtCct(camera_neutral, cm1, cm2, endpoint_cct_1, endpoint_cct_2, cct);
    if (!eval.valid_) {
      continue;
    }
    if (IsBetterAsShotEvaluation(eval, best)) {
      best = eval;
    }
    if (previous.valid_ && HasOppositeSigns(previous.residual_, eval.residual_)) {
      auto refined = RefineAsShotCctRoot(camera_neutral, cm1, cm2, endpoint_cct_1, endpoint_cct_2,
                                         previous, eval);
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
                                          double cct, double endpoint_cct_1, double endpoint_cct_2,
                                          const cv::Vec2d& white_xy,
                                          cv::Matx33d&     out_camera_to_xyz_d50) -> bool {
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

auto IsCustomWbMode(const std::string& mode) -> bool {
  return mode == "custom";
}

auto Fail(ColorTransformError error) -> ColorTransformResult {
  return ColorTransformResult{false, error, {}};
}

}  // namespace

void BindDevelopCameraProfile(DevelopPayload& payload, const RawRuntimeColorContext& imported) {
  auto& profile                     = payload.camera_profile;
  profile.dng_profile                   = imported.dng_profile_;
  profile.color_matrices_valid      = imported.color_matrices_valid_;
  profile.forward_matrices_valid    = imported.forward_matrices_valid_;
  profile.as_shot_neutral_valid     = imported.as_shot_neutral_valid_;
  profile.calibration_illuminants_valid = imported.calibration_illuminants_valid_;
  profile.color_matrix_1_cct        = imported.color_matrix_1_cct_;
  profile.color_matrix_2_cct        = imported.color_matrix_2_cct_;
  for (int i = 0; i < 9; ++i) {
    profile.color_matrix_1[static_cast<std::size_t>(i)]   = imported.color_matrix_1_[i];
    profile.color_matrix_2[static_cast<std::size_t>(i)]   = imported.color_matrix_2_[i];
    profile.forward_matrix_1[static_cast<std::size_t>(i)] = imported.forward_matrix_1_[i];
    profile.forward_matrix_2[static_cast<std::size_t>(i)] = imported.forward_matrix_2_[i];
  }
  for (int i = 0; i < 3; ++i) {
    profile.as_shot_neutral[static_cast<std::size_t>(i)] = imported.as_shot_neutral_[i];
    profile.cam_mul[static_cast<std::size_t>(i)]        = imported.cam_mul_[i];
  }

  DevelopPayload as_shot = payload;
  as_shot.wb_mode        = "as_shot";
  const auto solved      = ResolveDevelopColorTransform(as_shot);
  if (solved.ok) {
    payload.as_shot_cct  = solved.transform.resolved_cct;
    payload.as_shot_tint = solved.transform.resolved_tint;
  }
}

auto ResolveDevelopColorTransform(const DevelopPayload& develop) -> ColorTransformResult {
  cv::Matx33d cm1;
  cv::Matx33d cm2;
  double      endpoint_cct_1 = kCalibrationLowCCT;
  double      endpoint_cct_2 = kCalibrationHighCCT;
  const auto  endpoint_error =
      ResolveColorMatrixEndpoints(develop.camera_profile, cm1, cm2, endpoint_cct_1, endpoint_cct_2);
  if (endpoint_error != ColorTransformError::Ok) {
    return Fail(endpoint_error);
  }

  const auto& dng_profile = develop.camera_profile.dng_profile;
  if (dng_profile) {
    ApplyAnalogBalanceAndCameraCalibration(cm1.val, dng_profile->analog_balance.data(),
                                           dng_profile->camera_calibration_1.data());
    ApplyAnalogBalanceAndCameraCalibration(cm2.val, dng_profile->analog_balance.data(),
                                           dng_profile->camera_calibration_2.data());
  }

  cv::Matx33d fm1;
  cv::Matx33d fm2;
  const bool  has_forward_matrices =
      ResolveForwardMatrixEndpoints(develop.camera_profile, fm1, fm2);
  if (dng_profile && develop.camera_profile.forward_matrices_valid && !has_forward_matrices) {
    return Fail(ColorTransformError::NonFiniteMatrix);
  }

  cv::Vec2d selected_xy(kD50X, kD50Y);
  double    selected_cct  = develop.custom_cct;
  double    selected_tint = develop.custom_tint;
  if (!IsCustomWbMode(develop.wb_mode)) {
    if (!SolveAsShotWhiteXY(develop.camera_profile, cm1, cm2, endpoint_cct_1, endpoint_cct_2,
                            selected_xy, selected_cct, selected_tint)) {
      return Fail(ColorTransformError::InvalidAsShotNeutral);
    }
  } else {
    selected_cct  = ClampFinite(develop.custom_cct, kCustomCCTMin, kCustomCCTMax);
    selected_tint = ClampFinite(develop.custom_tint, kCustomTintMin, kCustomTintMax);
    selected_xy   = UVToXY(TemperatureTintToUV(selected_cct, selected_tint));
  }

  const cv::Matx33d xyz_to_camera =
      InterpolateColorMatrix(cm1, cm2, selected_cct, endpoint_cct_1, endpoint_cct_2);
  cv::Matx33d camera_to_xyz;
  if (!Invert3x3(xyz_to_camera, camera_to_xyz)) {
    return Fail(ColorTransformError::SingularCameraMatrix);
  }

  cv::Matx33d camera_to_xyz_d50;
  if (dng_profile) {
    cv::Vec3d    camera_white = xyz_to_camera * XYToXYZ(selected_xy);
    const double scale        = std::max({camera_white[0], camera_white[1], camera_white[2]});
    if (!std::isfinite(scale) || scale <= kValueEpsilon)
      return Fail(ColorTransformError::InvalidWhitePoint);
    if (has_forward_matrices) {
      const auto calibration =
          InterpolateColorMatrix(MatrixFromArray9(dng_profile->camera_calibration_1),
                                 MatrixFromArray9(dng_profile->camera_calibration_2), selected_cct,
                                 endpoint_cct_1, endpoint_cct_2);
      const auto& ab = dng_profile->analog_balance;
      cv::Matx33d individual_to_reference;
      if (!Invert3x3(cv::Matx33d::diag(cv::Vec3d(ab[0], ab[1], ab[2])) * calibration,
                     individual_to_reference))
        return Fail(ColorTransformError::SingularCameraMatrix);
      for (int c = 0; c < 3; ++c) camera_white[c] = std::clamp(camera_white[c] / scale, 0.001, 1.0);
      const auto reference_white = individual_to_reference * camera_white;
      if (std::any_of(std::begin(reference_white.val), std::end(reference_white.val),
                      [](double v) { return !std::isfinite(v) || v <= kValueEpsilon; })) {
        return Fail(ColorTransformError::InvalidWhitePoint);
      }
      // DNG normalizes each ForwardMatrix endpoint so unit camera RGB maps to D50.
      const auto d50 = XYToXYZ(cv::Vec2d(kD50X, kD50Y));
      for (auto* matrix : {&fm1, &fm2}) {
        for (int r = 0; r < 3; ++r) {
          const double sum = (*matrix)(r, 0) + (*matrix)(r, 1) + (*matrix)(r, 2);
          if (!std::isfinite(sum) || std::abs(sum) <= kValueEpsilon)
            return Fail(ColorTransformError::NonFiniteMatrix);
          for (int c = 0; c < 3; ++c) (*matrix)(r, c) *= d50[r] / sum;
        }
      }
      const auto forward =
          InterpolateColorMatrix(fm1, fm2, selected_cct, endpoint_cct_1, endpoint_cct_2);
      camera_to_xyz_d50 =
          forward *
          cv::Matx33d::diag(cv::Vec3d(1.0 / reference_white[0], 1.0 / reference_white[1],
                                      1.0 / reference_white[2])) *
          individual_to_reference;
    } else {
      camera_to_xyz_d50 =
          scale * BuildBradfordCAT(selected_xy, cv::Vec2d(kD50X, kD50Y)) * camera_to_xyz;
    }
  } else if (has_forward_matrices) {
    if (!BuildCameraToXyzD50FromForwardMatrix(cm1, cm2, fm1, fm2, selected_cct, endpoint_cct_1,
                                              endpoint_cct_2, selected_xy, camera_to_xyz_d50)) {
      return Fail(ColorTransformError::NonFiniteMatrix);
    }
  } else {
    camera_to_xyz_d50 = BuildBradfordCAT(selected_xy, cv::Vec2d(kD50X, kD50Y)) * camera_to_xyz;
  }
  if (!IsFiniteMatrix(camera_to_xyz_d50)) return Fail(ColorTransformError::NonFiniteMatrix);

  const cv::Matx33d cat_d50_to_d60 =
      BuildBradfordCAT(cv::Vec2d(kD50X, kD50Y), cv::Vec2d(kD60X, kD60Y));
  const cv::Matx33d xyz_d50_to_ap1 = kXyzD60ToAp1 * cat_d50_to_d60;
  const cv::Matx33d camera_to_ap1  = xyz_d50_to_ap1 * camera_to_xyz_d50;
  if (!IsFiniteMatrix(camera_to_ap1) || !IsFiniteMatrix(xyz_d50_to_ap1)) {
    return Fail(ColorTransformError::NonFiniteMatrix);
  }

  DevelopColorTransform transform;
  StoreMatrix(camera_to_xyz, transform.camera_to_xyz);
  StoreMatrix(camera_to_xyz_d50, transform.camera_to_xyz_d50);
  StoreMatrix(xyz_d50_to_ap1, transform.xyz_d50_to_ap1);
  StoreMatrix(camera_to_ap1, transform.camera_to_ap1);
  transform.resolved_cct  = static_cast<float>(ClampFinite(selected_cct, kCustomCCTMin, kCustomCCTMax));
  transform.resolved_tint = static_cast<float>(ClampFinite(selected_tint, kCustomTintMin, kCustomTintMax));
  return ColorTransformResult{true, ColorTransformError::Ok, transform};
}

auto ColorTransformErrorMessage(ColorTransformError error) -> std::string_view {
  switch (error) {
    case ColorTransformError::Ok:
      return "ok";
    case ColorTransformError::MissingCameraMatrices:
      return "missing camera matrices";
    case ColorTransformError::SingularCameraMatrix:
      return "singular camera matrix";
    case ColorTransformError::NonFiniteMatrix:
      return "non-finite color matrix";
    case ColorTransformError::InvalidAsShotNeutral:
      return "invalid as-shot neutral";
    case ColorTransformError::InvalidWhitePoint:
      return "invalid white point";
  }
  return "color transform error";
}

}  // namespace alcedo
