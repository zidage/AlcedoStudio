//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/operators/cpu/color_space_conv.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <utility>

#include "decoders/processor/operators/cpu/raw_proc_utils.hpp"

namespace alcedo {
namespace CPU {
namespace {
// -----------------------------------------------------------------------------
// Helper utilities kept internal to this translation unit
// -----------------------------------------------------------------------------
constexpr float           kMinGain          = 1e-6f;
constexpr float           kFallbackMinGreen = 0.1f;

/**
 * @brief Builds a diagonal 3x3 matrix from the provided RGB scalars.
 */
static inline cv::Matx33f BuildDiagonal(float r, float g, float b) {
  return cv::Matx33f(r, 0.f, 0.f, 0.f, g, 0.f, 0.f, 0.f, b);
}

/**
 * @brief Guarded division that protects against extremely small denominators.
 */
static inline float SafeDivide(float numerator, float denominator) {
  return numerator / std::max(denominator, kMinGain);
}

/**
 * @brief Converts a raw multiplier triplet (R, G, B) into a diagonal matrix normalized by G.
 */
static inline cv::Matx33f NormalizeMultipliers(const float* mul) {
  float g = std::max(mul[1], kMinGain);
  return BuildDiagonal(SafeDivide(mul[0], g), 1.f, SafeDivide(mul[2], g));
}

/**
 * @brief Converts camera → XYZ coefficients into a cv::Matx33f.
 */
static inline cv::Matx33f BuildCamMatrix(const float cam_xyz[][3]) {
  return cv::Matx33f(cam_xyz[0][0], cam_xyz[0][1], cam_xyz[0][2], cam_xyz[1][0], cam_xyz[1][1],
                     cam_xyz[1][2], cam_xyz[2][0], cam_xyz[2][1], cam_xyz[2][2]);
}

/**
 * @brief Computes the XYZ → camera matrix and returns its inverse (camera → XYZ).
 */
static inline cv::Matx33f ComputeCam2Xyz(const cv::Matx33f& normalized_pre_mul,
                                         const cv::Matx33f& cam_xyz_matrix) {
  cv::Matx33f xyz_to_cam = normalized_pre_mul * cam_xyz_matrix;
  return xyz_to_cam.inv();
}

/**
 * @brief Builds the preprocessor-to-target gain matrix from a target gain and the baked-in gain.
 */
static inline cv::Matx33f BuildPreToTargetMatrix(const cv::Matx33f& target_gain,
                                                 const cv::Matx33f& normalized_pre_mul) {
  return BuildDiagonal(SafeDivide(target_gain(0, 0), normalized_pre_mul(0, 0)),
                       SafeDivide(target_gain(1, 1), normalized_pre_mul(1, 1)),
                       SafeDivide(target_gain(2, 2), normalized_pre_mul(2, 2)));
}

/**
 * @brief Applies the full color-space transform: camera → XYZ → ACES2065 with the desired gains.
 */
static inline void ApplyWithPreToTarget(cv::Mat& img, const cv::Matx33f& cam2xyz,
                                        const cv::Matx33f& pre_to_target) {
  static const cv::Matx33f M_CAT_D65_to_D60 = {1.01303491f,  0.00610526f, -0.01497094f,
                                               0.00769823f,  0.99816335f, -0.00503204f,
                                               -0.00284132f, 0.00468516f, 0.92450614f};

  static const cv::Matx33f xyz2aces2065     = {1.0498110175f,  0.0000000000f, -0.0000974845f,
                                               -0.4959030231f, 1.3733130458f, 0.0982400361f,
                                               0.0000000000f,  0.0000000000f, 0.9912520182f};
  cv::Matx33f              total_matrix = xyz2aces2065 * M_CAT_D65_to_D60 * cam2xyz * pre_to_target;
  cv::transform(img, img, total_matrix);
}

/**
 * @brief Converts integer WB coefficients (R,G,B) into a normalized diagonal matrix.
 */
static inline cv::Matx33f BuildWbMatrixFromIntegers(const int wb_mul[4]) {
  float g = std::max(static_cast<float>(wb_mul[1]), kMinGain);
  return BuildDiagonal(static_cast<float>(wb_mul[0]) / g, 1.f, static_cast<float>(wb_mul[2]) / g);
}

/**
 * @brief Element-wise divide on diagonal matrices (used for calibration factors).
 */
static inline cv::Matx33f ElementWiseDivide(const cv::Matx33f& a, const cv::Matx33f& b) {
  return BuildDiagonal(SafeDivide(a(0, 0), b(0, 0)), SafeDivide(a(1, 1), b(1, 1)),
                       SafeDivide(a(2, 2), b(2, 2)));
}

static inline bool HasValidCamXyz(const float cam_xyz[][3]) {
  // LibRaw sometimes provides a zeroed cam_xyz when unavailable.
  float abs_sum = 0.f;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float v = cam_xyz[r][c];
      if (!std::isfinite(v)) return false;
      abs_sum += std::abs(v);
    }
  }
  return abs_sum > 0.f;
}

static inline bool HasValidRgbCam(const float rgb_cam[][4]) {
  // rgb_cam is expected to be present for most raws; treat all-zeros as invalid.
  float abs_sum = 0.f;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float v = rgb_cam[r][c];
      if (!std::isfinite(v)) return false;
      abs_sum += std::abs(v);
    }
  }
  return abs_sum > 0.f;
}

static inline cv::Matx33f BuildCamToSrgbMatrix(const float rgb_cam[][4]) {
  // rgb_cam is a 3x4 matrix in LibRaw; the 4th column is an offset in some pipelines.
  // Our pipeline is linear and uses only the 3x3 part.
  return cv::Matx33f(rgb_cam[0][0], rgb_cam[0][1], rgb_cam[0][2], rgb_cam[1][0], rgb_cam[1][1],
                     rgb_cam[1][2], rgb_cam[2][0], rgb_cam[2][1], rgb_cam[2][2]);
}

/**
 * @brief Applies the transform: cameraRGB -> linear sRGB (via rgb_cam) -> linear ACES2065-1 (AP0).
 *
 * Notes:
 * - rgb_cam path does NOT pre-multiply by normalized_pre_mul (per LibRaw/dcraw convention).
 * - The fixed sRGB->ACES(AP0) matrix below includes D65->D60 chromatic adaptation.
 */
static inline void ApplyFromCamToSrgbThenAces(cv::Mat& img, const cv::Matx33f& cam2srgb,
                                             const cv::Matx33f& pre_to_target) {
  // Linear sRGB (D65) -> XYZ (D65)
  static const cv::Matx33f srgb2xyz_d65 = {
      0.41239080f, 0.35758434f, 0.18048079f, 0.21263901f, 0.71516868f,
      0.07219232f, 0.01933082f, 0.11919478f, 0.95053215f};

  static const cv::Matx33f M_CAT_D65_to_D60 = {1.01303491f,  0.00610526f, -0.01497094f,
                                               0.00769823f,  0.99816335f, -0.00503204f,
                                               -0.00284132f, 0.00468516f, 0.92450614f};

  // XYZ (D60) -> ACES2065-1 (AP0)
  static const cv::Matx33f xyz2aces2065 = {1.0498110175f,  0.0000000000f, -0.0000974845f,
                                           -0.4959030231f, 1.3733130458f, 0.0982400361f,
                                           0.0000000000f,  0.0000000000f, 0.9912520182f};

  // Preset: linear sRGB (D65) -> linear ACES2065-1 (AP0, D60)
  static const cv::Matx33f srgb2aces2065_d60 = xyz2aces2065 * M_CAT_D65_to_D60 * srgb2xyz_d65;

  cv::Matx33f total_matrix = srgb2aces2065_d60 * cam2srgb * pre_to_target;
  cv::transform(img, img, total_matrix);
}
}  // namespace

void ApplyColorMatrix(cv::Mat& img, const float rgb_cam[][4], const float* pre_mul,
                      const float* cam_mul, const float cam_xyz[][3]) {
  (void)rgb_cam;  // Current pipeline does not reuse rgb_cam directly.

  cv::Matx33f normalized_pre_mul = NormalizeMultipliers(pre_mul);
  cv::Matx33f normalized_cam_mul = NormalizeMultipliers(cam_mul);
  // cv::Matx33f pre_to_cam_matrix =
  //     BuildDiagonal(SafeDivide(normalized_cam_mul(0, 0), normalized_pre_mul(0, 0)),
  //                   SafeDivide(normalized_cam_mul(1, 1), normalized_pre_mul(1, 1)),
  //                   SafeDivide(normalized_cam_mul(2, 2), normalized_pre_mul(2, 2)));
  cv::Matx33f pre_to_cam_matrix  = BuildDiagonal(1.f, 1.f, 1.f);

  if (HasValidCamXyz(cam_xyz)) {
    cv::Matx33f cam_xyz_matrix = BuildCamMatrix(cam_xyz);
    cv::Matx33f cam2xyz_matrix = ComputeCam2Xyz(normalized_pre_mul, cam_xyz_matrix);
    ApplyWithPreToTarget(img, cam2xyz_matrix, pre_to_cam_matrix);
    return;
  }

  // Fallback: camera -> linear sRGB using rgb_cam, then to linear ACES AP0 (D60).
  if (!HasValidRgbCam(rgb_cam)) {
    throw std::runtime_error("ApplyColorMatrix: missing both cam_xyz and rgb_cam matrices");
  }

  cv::Matx33f cam2srgb_matrix = BuildCamToSrgbMatrix(rgb_cam);
  ApplyFromCamToSrgbThenAces(img, cam2srgb_matrix, pre_to_cam_matrix);
}

static inline std::pair<float, float> PlanckianLocusApprox(int target_K) {
  double T = static_cast<double>(target_K);
  if (T < 1667.0 || T > 25000.0)
    throw std::runtime_error("CCT out of supported range (1667K - 25000K)");

  double x;
  if (T <= 4000.0) {
    x = -0.2661239e9 / (T * T * T) - 0.2343580e6 / (T * T) + 0.8776956e3 / T + 0.179910;
  } else {
    x = -3.0258469e9 / (T * T * T) + 2.1070379e6 / (T * T) + 0.2226347e3 / T + 0.240390;
  }

  double y = -3.0 * x * x + 2.87 * x - 0.275;
  return {static_cast<float>(x), static_cast<float>(y)};
}

static inline cv::Matx33f GetGainMatrixForWb(int target_K, cv::Matx33f cam_xyz) {
  auto        xy = PlanckianLocusApprox(target_K);
  float       X  = xy.first / xy.second;
  float       Y  = 1.0f;
  float       Z  = (1.0f - xy.first - xy.second) / xy.second;
  cv::Matx31f target_wp(X, Y, Z);
  cv::Matx31f cam_response = cam_xyz * target_wp;

  float       g_r          = SafeDivide(1.f, cam_response(0, 0) + kMinGain);
  float       g_g          = SafeDivide(1.f, cam_response(1, 0) + kMinGain);
  float       g_b          = SafeDivide(1.f, cam_response(2, 0) + kMinGain);

  return BuildDiagonal(g_r / g_g, 1.f, g_b / g_g);
}

void ApplyColorMatrix(cv::Mat& img, const float rgb_cam[][4], const float* pre_mul,
                      const float* cam_mul, const int wb_coeffs[][4],
                      std::pair<int, int> user_temp_indices, int user_wb,
                      const float cam_xyz[][3]) {
  (void)rgb_cam;
  (void)cam_mul;

  const bool use_cam_xyz = HasValidCamXyz(cam_xyz);

  cv::Matx33f normalized_pre_mul = NormalizeMultipliers(pre_mul);

  // "Apply gain" is implemented in two backends:
  // - cam_xyz backend (camera -> XYZ -> ACES) keeps the existing behavior.
  // - rgb_cam backend (camera -> linear sRGB -> ACES) avoids pre-multiplying normalized_pre_mul.
  std::function<void(const cv::Matx33f&)> apply_gain;
  std::function<void(int)>               apply_planckian_gain;

  cv::Matx33f cam_xyz_matrix;
  cv::Matx33f cam2xyz_matrix;
  cv::Matx33f cam2srgb_matrix;

  if (use_cam_xyz) {
    cam_xyz_matrix = BuildCamMatrix(cam_xyz);
    cam2xyz_matrix = ComputeCam2Xyz(normalized_pre_mul, cam_xyz_matrix);

    apply_gain = [&](const cv::Matx33f& gain_matrix) {
      cv::Matx33f pre_to_user_matrix = BuildPreToTargetMatrix(gain_matrix, normalized_pre_mul);
      ApplyWithPreToTarget(img, cam2xyz_matrix, pre_to_user_matrix);
    };

    apply_planckian_gain = [&](int temperature) {
      cv::Matx33f planck_gain = GetGainMatrixForWb(temperature, cam_xyz_matrix);
      apply_gain(planck_gain);
    };
  } else {
    if (!HasValidRgbCam(rgb_cam)) {
      throw std::runtime_error("ApplyColorMatrix(user_wb): missing both cam_xyz and rgb_cam");
    }

    cam2srgb_matrix = BuildCamToSrgbMatrix(rgb_cam);

    apply_gain = [&](const cv::Matx33f& gain_matrix) {
      // rgb_cam path: do not treat normalized_pre_mul as baked-in.
      cv::Matx33f pre_to_user_matrix = gain_matrix;
      ApplyFromCamToSrgbThenAces(img, cam2srgb_matrix, pre_to_user_matrix);
    };

    // No cam_xyz: we cannot derive a Planckian WB from camera spectral response.
    // Best-effort fallback is to skip Planckian and rely on available WB_Coeffs.
    apply_planckian_gain = [&](int /*temperature*/) {
      apply_gain(BuildDiagonal(1.f, 1.f, 1.f));
    };
  }

  int        idx1      = user_temp_indices.first;
  int        idx2      = user_temp_indices.second;

  const int* wb_mul1   = wb_coeffs[idx1];
  const int* wb_mul2   = wb_coeffs[idx2];

  bool       wb1_valid = wb_mul1[1] >= kFallbackMinGreen;
  bool       wb2_valid = wb_mul2[1] >= kFallbackMinGreen;

  if (idx1 == idx2) {
    if (!wb1_valid) {
      apply_planckian_gain(user_wb);
      return;
    }
    apply_gain(BuildWbMatrixFromIntegers(wb_mul1));
    return;
  }

  if (!wb1_valid && !wb2_valid) {
    apply_planckian_gain(user_wb);
    return;
  }

  if (!wb1_valid || !wb2_valid) {
    int         valid_idx          = wb1_valid ? idx1 : idx2;
    const int*  wb_mul             = wb_coeffs[valid_idx];
    cv::Matx33f anchor_gain        = BuildWbMatrixFromIntegers(wb_mul);

    if (use_cam_xyz) {
      int         anchor_temp        = CPU::GetTempForWBIndices(valid_idx);
      cv::Matx33f theoretical_anchor = GetGainMatrixForWb(anchor_temp, cam_xyz_matrix);
      cv::Matx33f calibration_factor = ElementWiseDivide(anchor_gain, theoretical_anchor);

      cv::Matx33f target_theoretical = GetGainMatrixForWb(user_wb, cam_xyz_matrix);
      cv::Matx33f final_gain         = target_theoretical.mul(calibration_factor);

      apply_gain(final_gain);
    } else {
      // Best-effort: only one WB_Coeffs entry is usable; apply it directly.
      apply_gain(anchor_gain);
    }
    return;
  }

  float       temp1      = static_cast<float>(CPU::GetTempForWBIndices(idx1));
  float       temp2      = static_cast<float>(CPU::GetTempForWBIndices(idx2));
  float       ratio      = SafeDivide(static_cast<float>(user_wb) - temp1, temp2 - temp1);

  cv::Matx33f wb_matrix1 = BuildWbMatrixFromIntegers(wb_mul1);
  cv::Matx33f wb_matrix2 = BuildWbMatrixFromIntegers(wb_mul2);
  cv::Matx33f interpolated_gain =
      wb_matrix1 * (1.f - ratio) + wb_matrix2 * ratio;  // Linear interpolation.

  apply_gain(interpolated_gain);
}
};  // namespace CPU
};  // namespace alcedo