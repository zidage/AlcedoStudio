//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

namespace alcedo {

inline void SetIdentity3x3(double* out) {
  out[0] = 1.0;
  out[1] = 0.0;
  out[2] = 0.0;
  out[3] = 0.0;
  out[4] = 1.0;
  out[5] = 0.0;
  out[6] = 0.0;
  out[7] = 0.0;
  out[8] = 1.0;
}

inline void SetDiagonal3x3(double r, double g, double b, double* out) {
  SetIdentity3x3(out);
  out[0] = r;
  out[4] = g;
  out[8] = b;
}

inline void Multiply3x3RowMajor(const double* a, const double* b, double* out) {
  double tmp[9];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      tmp[r * 3 + c] = a[r * 3 + 0] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
  }
  std::copy(tmp, tmp + 9, out);
}

inline auto IsFinitePositive3(const double* values) -> bool {
  return values != nullptr && std::isfinite(values[0]) && values[0] > 0.0 &&
         std::isfinite(values[1]) && values[1] > 0.0 && std::isfinite(values[2]) && values[2] > 0.0;
}

inline auto IsFinite3x3(const double* matrix) -> bool {
  if (matrix == nullptr) {
    return false;
  }
  for (int i = 0; i < 9; ++i) {
    if (!std::isfinite(matrix[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Compose DNG XYZtoCamera = AnalogBalance * CameraCalibration * ColorMatrix.
 *
 * Adobe's dng_color_spec bakes AnalogBalance and CameraCalibration into ColorMatrix
 * before NeutralToXY. AnalogBalance is a 3-channel diagonal. Invalid analog gains or
 * calibration matrices are treated as identity so ColorMatrix is left unchanged.
 */
inline void ApplyAnalogBalanceAndCameraCalibration(double* color_matrix,
                                                   const double analog_balance[3],
                                                   const double camera_calibration[9]) {
  if (color_matrix == nullptr || !IsFinite3x3(color_matrix)) {
    return;
  }
  double analog_matrix[9];
  double calibration_matrix[9];
  if (IsFinitePositive3(analog_balance)) {
    SetDiagonal3x3(analog_balance[0], analog_balance[1], analog_balance[2], analog_matrix);
  } else {
    SetIdentity3x3(analog_matrix);
  }
  if (IsFinite3x3(camera_calibration)) {
    std::copy(camera_calibration, camera_calibration + 9, calibration_matrix);
  } else {
    SetIdentity3x3(calibration_matrix);
  }
  double calibrated[9];
  Multiply3x3RowMajor(calibration_matrix, color_matrix, calibrated);
  Multiply3x3RowMajor(analog_matrix, calibrated, color_matrix);
}

/// Adobe SDK applies CameraCalibration only when the two signatures compare equal.
inline auto CameraCalibrationSignaturesMatch(std::string_view camera_signature,
                                             std::string_view profile_signature) -> bool {
  return camera_signature == profile_signature;
}

}  // namespace alcedo
