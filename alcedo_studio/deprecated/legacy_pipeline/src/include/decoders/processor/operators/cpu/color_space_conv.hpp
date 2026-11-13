//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core.hpp>

namespace alcedo {
namespace CPU {

void ApplyColorMatrix(cv::Mat& img, const float rgb_cam[][4], const float* pre_mul,
                      const float* cam_mul, const float cam_xyz[][3]);

void ApplyColorMatrix(cv::Mat& img, const float rgb_cam[][4], const float* pre_mul,
                      const float* cam_mul, const int wb_coeffs[][4],
                      std::pair<int, int> user_temp_indices, int user_wb, const float cam_xyz[][3]);
};  // namespace CPU
};  // namespace alcedo