//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/operators/cpu/debayer_ahd.hpp"

#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgproc.hpp>

namespace alcedo {
namespace CPU {
void BayerRGGB2RGB_AHD(cv::Mat& bayer) {
  static int remap[4] = {0, 1, 3, 2};
  const int  h        = bayer.rows;
  const int  w        = bayer.cols;

  cv::Mat1f  mask_R(h, w, 0.0f);
  cv::Mat1f  mask_G_r(h, w, 0.0f);
  cv::Mat1f  mask_G_b(h, w, 0.0f);
  cv::Mat1f  mask_B(h, w, 0.0f);

  // build masks using forEacll
  bayer.forEach<float>([&](float&, const int* pos) {
    int y           = pos[0];
    int x           = pos[1];
    int color_index = remap[(((y % 2) * 2) + (x % 2)) % 4];  // assume 0:R,1:G_r,2:B,3:G_b
    switch (color_index) {
      case 0:
        mask_R(y, x) = 1.0f;
        break;
      case 1:
        mask_G_r(y, x) = 1.0f;
        break;
      case 2:
        mask_B(y, x) = 1.0f;
        break;
      case 3:
        mask_G_b(y, x) = 1.0f;
        break;
      default:
        break;
    }
  });

  cv::Mat1f raw(bayer);

  cv::Mat1f G_final = bayer.clone();
#pragma omp parallel for schedule(dynamic)
  for (int y = 2; y < h - 2; ++y) {
    for (int x = 2; x < w - 2; ++x) {
      if (mask_R(y, x) > 0.5f || mask_B(y, x) > 0.5f) {
        float center  = raw(y, x);
        float h_avg   = 0.5f * (raw(y, x - 1) + raw(y, x + 1));
        float h_diff  = 0.25f * (2.0f * center - raw(y, x - 2) - raw(y, x + 2));
        // G_h(y, x)     = h_avg + h_diff;

        float v_avg   = 0.5f * (raw(y - 1, x) + raw(y + 1, x));
        float v_diff  = 0.25f * (2.0f * center - raw(y - 2, x) - raw(y + 2, x));
        // G_v(y, x)     = v_avg + v_diff;

        // Calculate G final now
        float Dh      = std::abs(raw(y, x - 1) - raw(y, x + 1));
        float Dv      = std::abs(raw(y - 1, x) - raw(y + 1, x));
        G_final(y, x) = (Dh < Dv) ? (h_avg + h_diff) : (v_avg + v_diff);
      }
    }
  }

  // initialize R_final and B_final (only at their native positions)
  cv::Mat1f R_final = bayer.mul(mask_R);
  cv::Mat1f B_final = bayer.mul(mask_B);

// Fill missing R and B using pattern-aware interpolation with forEach
#pragma omp parallel for schedule(dynamic)
  for (int y = 1; y < h - 1; ++y) {
    for (int x = 1; x < w - 1; ++x) {
      // --- fill missing R ---
      if (mask_R(y, x) < 0.5f) {
        float estimateR = 0.0f;
        if (mask_G_r(y, x) > 0.5f) {
          // G on R row: R neighbors are left/right
          float left  = R_final(y, x - 1) - G_final(y, x - 1);
          float right = R_final(y, x + 1) - G_final(y, x + 1);
          estimateR   = G_final(y, x) + 0.5f * (left + right);
        } else if (mask_G_b(y, x) > 0.5f) {
          // G on B row: R neighbors are up/down
          float up   = R_final(y - 1, x) - G_final(y - 1, x);
          float down = R_final(y + 1, x) - G_final(y + 1, x);
          estimateR  = G_final(y, x) + 0.5f * (up + down);
        } else if (mask_B(y, x) > 0.5f) {
          // At a B pixel: R is on diagonals
          float d1  = R_final(y - 1, x - 1) - G_final(y - 1, x - 1);
          float d2  = R_final(y - 1, x + 1) - G_final(y - 1, x + 1);
          float d3  = R_final(y + 1, x - 1) - G_final(y + 1, x - 1);
          float d4  = R_final(y + 1, x + 1) - G_final(y + 1, x + 1);
          estimateR = G_final(y, x) + 0.25f * (d1 + d2 + d3 + d4);
        } else {
          // fallback: simple average of immediate neighbors
          float left  = R_final(y, x - 1) - G_final(y, x - 1);
          float right = R_final(y, x + 1) - G_final(y, x + 1);
          float up    = R_final(y - 1, x) - G_final(y - 1, x);
          float down  = R_final(y + 1, x) - G_final(y + 1, x);
          estimateR   = G_final(y, x) + 0.25f * (left + right + up + down);
        }
        R_final(y, x) = estimateR;
      }

      // --- fill missing B ---
      if (mask_B(y, x) < 0.5f) {
        float estimateB = 0.0f;
        if (mask_G_b(y, x) > 0.5f) {
          // G on B row: B neighbors are left/right
          float left  = B_final(y, x - 1) - G_final(y, x - 1);
          float right = B_final(y, x + 1) - G_final(y, x + 1);
          estimateB   = G_final(y, x) + 0.5f * (left + right);
        } else if (mask_G_r(y, x) > 0.5f) {
          // G on R row: B neighbors are up/down
          float up   = B_final(y - 1, x) - G_final(y - 1, x);
          float down = B_final(y + 1, x) - G_final(y + 1, x);
          estimateB  = G_final(y, x) + 0.5f * (up + down);
        } else if (mask_R(y, x) > 0.5f) {
          // At an R pixel: B is on diagonals
          float d1  = B_final(y - 1, x - 1) - G_final(y - 1, x - 1);
          float d2  = B_final(y - 1, x + 1) - G_final(y - 1, x + 1);
          float d3  = B_final(y + 1, x - 1) - G_final(y + 1, x - 1);
          float d4  = B_final(y + 1, x + 1) - G_final(y + 1, x + 1);
          estimateB = G_final(y, x) + 0.25f * (d1 + d2 + d3 + d4);
        } else {
          // fallback
          float left  = B_final(y, x - 1) - G_final(y, x - 1);
          float right = B_final(y, x + 1) - G_final(y, x + 1);
          float up    = B_final(y - 1, x) - G_final(y - 1, x);
          float down  = B_final(y + 1, x) - G_final(y + 1, x);
          estimateB   = G_final(y, x) + 0.25f * (left + right + up + down);
        }
        B_final(y, x) = estimateB;
      }
    }
  }

  // merge channels into a 3-channel RGB image
  std::vector<cv::Mat> channels = {R_final, G_final, B_final};
  cv::merge(channels, bayer);
}
};  // namespace CPU
};  // namespace alcedo