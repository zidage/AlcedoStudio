//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
//  This file also contains material subject to the upstream notices below.

#include "decoders/processor/operators/cpu/debayer_rcd.hpp"

#include <opencv2/core/hal/interface.h>

#include <algorithm>  // for std::max, std::clamp, std::abs
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>

namespace alcedo {
namespace CPU {

static int fc[2][2] = {{0, 1}, {1, 2}};  // R=0, G1=1, B=2, G2=1
#ifdef FC
#undef FC
#endif
#define FC(y, x) fc[(y) & 1][(x) & 1]

/**
 * @brief Adapted from https://github.com/LuisSR/RCD-Demosaicing/blob/master/rcd_demosaicing.c
 *
 * RATIO CORRECTED DEMOSAICING
 * Luis Sanz Rodríguez (luis.sanz.rodriguez(at)gmail(dot)com)
 *
 * Release 2.3 @ 171125
 *
 * @param bayer Input Bayer pattern image (CV_32F)
 */
void BayerRGGB2RGB_RCD(cv::Mat& bayer) {
  cv::Mat1f bayer_float;
  if (bayer.type() != CV_32F) {
    bayer.convertTo(bayer_float, CV_32F);
  } else {
    bayer_float = bayer.clone();
  }

  int                    width  = bayer_float.cols;
  int                    height = bayer_float.rows;

  int                    w1 = width, w2 = 2 * width, w3 = 3 * width, w4 = 4 * width;

  cv::Mat3f              output(height, width, cv::Vec3f(0, 0, 0));

  cv::Vec3f*             rgb = (cv::Vec3f*)output.data;
  float*                 cfa = (float*)bayer_float.data;

  // Tolerance value
  static constexpr float eps = 1e-5f, eps_sq = 1e-10f;

  cv::Mat1f              VH_dir(height, width, 0.0f);
  cv::Mat1f              PQ_dir(height, width, 0.0f);
  cv::Mat1f              low_pass(height, width, 0.0f);

  float*                 VH_Dir = (float*)VH_dir.data;
  float*                 PQ_Dir = (float*)PQ_dir.data;
  float*                 lpf    = (float*)low_pass.data;

  // Copy value into output channels
#pragma omp parallel for
  for (int row = 0; row < height; row++) {
    for (int col = 0; col < width; col++) {
      int indx                = row * width + col;
      rgb[indx][FC(row, col)] = cfa[indx];
    }
  }

#pragma omp parallel for
  for (int row = 4; row < height - 4; row++) {
    for (int col = 4; col < width - 4; col++) {
      int   indx   = row * width + col;

      // Caluculate directional gradients
      float V_stat = std::max(
          -18.f * cfa[indx] * cfa[indx - w1] - 18.f * cfa[indx] * cfa[indx + w1] -
              36.f * cfa[indx] * cfa[indx - w2] - 36.f * cfa[indx] * cfa[indx + w2] +
              18.f * cfa[indx] * cfa[indx - w3] + 18.f * cfa[indx] * cfa[indx + w3] -
              2.f * cfa[indx] * cfa[indx - w4] - 2.f * cfa[indx] * cfa[indx + w4] +
              38.f * cfa[indx] * cfa[indx] - 70.f * cfa[indx - w1] * cfa[indx + w1] -
              12.f * cfa[indx - w1] * cfa[indx - w2] + 24.f * cfa[indx - w1] * cfa[indx + w2] -
              38.f * cfa[indx - w1] * cfa[indx - w3] + 16.f * cfa[indx - w1] * cfa[indx + w3] +
              12.f * cfa[indx - w1] * cfa[indx - w4] - 6.f * cfa[indx - w1] * cfa[indx + w4] +
              46.f * cfa[indx - w1] * cfa[indx - w1] + 24.f * cfa[indx + w1] * cfa[indx - w2] -
              12.f * cfa[indx + w1] * cfa[indx + w2] + 16.f * cfa[indx + w1] * cfa[indx - w3] -
              38.f * cfa[indx + w1] * cfa[indx + w3] - 6.f * cfa[indx + w1] * cfa[indx - w4] +
              12.f * cfa[indx + w1] * cfa[indx + w4] + 46.f * cfa[indx + w1] * cfa[indx + w1] +
              14.f * cfa[indx - w2] * cfa[indx + w2] - 12.f * cfa[indx - w2] * cfa[indx + w3] -
              2.f * cfa[indx - w2] * cfa[indx - w4] + 2.f * cfa[indx - w2] * cfa[indx + w4] +
              11.f * cfa[indx - w2] * cfa[indx - w2] - 12.f * cfa[indx + w2] * cfa[indx - w3] +
              2.f * cfa[indx + w2] * cfa[indx - w4] - 2.f * cfa[indx + w2] * cfa[indx + w4] +
              11.f * cfa[indx + w2] * cfa[indx + w2] + 2.f * cfa[indx - w3] * cfa[indx + w3] -
              6.f * cfa[indx - w3] * cfa[indx - w4] + 10.f * cfa[indx - w3] * cfa[indx - w3] -
              6.f * cfa[indx + w3] * cfa[indx + w4] + 10.f * cfa[indx + w3] * cfa[indx + w3] +
              1.f * cfa[indx - w4] * cfa[indx - w4] + 1.f * cfa[indx + w4] * cfa[indx + w4],
          eps_sq);
      float H_stat =
          std::max(-18.f * cfa[indx] * cfa[indx - 1] - 18.f * cfa[indx] * cfa[indx + 1] -
                       36.f * cfa[indx] * cfa[indx - 2] - 36.f * cfa[indx] * cfa[indx + 2] +
                       18.f * cfa[indx] * cfa[indx - 3] + 18.f * cfa[indx] * cfa[indx + 3] -
                       2.f * cfa[indx] * cfa[indx - 4] - 2.f * cfa[indx] * cfa[indx + 4] +
                       38.f * cfa[indx] * cfa[indx] - 70.f * cfa[indx - 1] * cfa[indx + 1] -
                       12.f * cfa[indx - 1] * cfa[indx - 2] + 24.f * cfa[indx - 1] * cfa[indx + 2] -
                       38.f * cfa[indx - 1] * cfa[indx - 3] + 16.f * cfa[indx - 1] * cfa[indx + 3] +
                       12.f * cfa[indx - 1] * cfa[indx - 4] - 6.f * cfa[indx - 1] * cfa[indx + 4] +
                       46.f * cfa[indx - 1] * cfa[indx - 1] + 24.f * cfa[indx + 1] * cfa[indx - 2] -
                       12.f * cfa[indx + 1] * cfa[indx + 2] + 16.f * cfa[indx + 1] * cfa[indx - 3] -
                       38.f * cfa[indx + 1] * cfa[indx + 3] - 6.f * cfa[indx + 1] * cfa[indx - 4] +
                       12.f * cfa[indx + 1] * cfa[indx + 4] + 46.f * cfa[indx + 1] * cfa[indx + 1] +
                       14.f * cfa[indx - 2] * cfa[indx + 2] - 12.f * cfa[indx - 2] * cfa[indx + 3] -
                       2.f * cfa[indx - 2] * cfa[indx - 4] + 2.f * cfa[indx - 2] * cfa[indx + 4] +
                       11.f * cfa[indx - 2] * cfa[indx - 2] - 12.f * cfa[indx + 2] * cfa[indx - 3] +
                       2.f * cfa[indx + 2] * cfa[indx - 4] - 2.f * cfa[indx + 2] * cfa[indx + 4] +
                       11.f * cfa[indx + 2] * cfa[indx + 2] + 2.f * cfa[indx - 3] * cfa[indx + 3] -
                       6.f * cfa[indx - 3] * cfa[indx - 4] + 10.f * cfa[indx - 3] * cfa[indx - 3] -
                       6.f * cfa[indx + 3] * cfa[indx + 4] + 10.f * cfa[indx + 3] * cfa[indx + 3] +
                       1.f * cfa[indx - 4] * cfa[indx - 4] + 1.f * cfa[indx + 4] * cfa[indx + 4],
                   eps_sq);
      VH_Dir[indx] = V_stat / (V_stat + H_stat);
    }
  }

  // Step 2: Calulate the low pass filter
  // Step 2.1 : Low pass filter incorporating green, red and blue local samples from the raw data
#pragma omp parallel for
  for (int row = 2; row < height - 2; row++) {
    for (int col = 2 + (FC(row, 0) & 1); col < width - 2; col += 2) {
      int indx  = row * width + col;
      lpf[indx] = 0.25f * cfa[indx] +
                  0.125f * (cfa[indx - w1] + cfa[indx + w1] + cfa[indx - 1] + cfa[indx + 1]) +
                  0.0625f * (cfa[indx - w1 - 1] + cfa[indx - w1 + 1] + cfa[indx + w1 - 1] +
                             cfa[indx + w1 + 1]);
    }
  }

  // Step 3: Populate the green channel
  // Step 3.1: Populate the green channel at blue and red CFA positions
#pragma omp parallel for
  for (int row = 4; row < height - 4; row++) {
    for (int col = 4 + (FC(row, 0) & 1); col < width - 4; col += 2) {
      int   indx           = row * width + col;
      float VH_central_val = VH_Dir[indx];
      float VH_neigh_val   = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] +
                                    VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
      float VH_disc        = (std::abs(0.5f - VH_central_val) < std::abs(0.5f - VH_neigh_val))
                                 ? VH_neigh_val
                                 : VH_central_val;

      // Cardinal gradients
      float N_grad =
          eps + std::abs(cfa[indx - w1] - cfa[indx + w1]) + std::abs(cfa[indx] - cfa[indx - w2]) +
          std::abs(cfa[indx - w1] - cfa[indx - w3]) + std::abs(cfa[indx - w2] - cfa[indx - w4]);
      float S_grad =
          eps + std::abs(cfa[indx + w1] - cfa[indx - w1]) + std::abs(cfa[indx] - cfa[indx + w2]) +
          std::abs(cfa[indx + w1] - cfa[indx + w3]) + std::abs(cfa[indx + w2] - cfa[indx + w4]);
      float W_grad = eps + std::abs(cfa[indx - 1] - cfa[indx + 1]) +
                     std::abs(cfa[indx] - cfa[indx - 2]) + std::abs(cfa[indx - 1] - cfa[indx - 3]) +
                     std::abs(cfa[indx - 2] - cfa[indx - 4]);
      float E_grad = eps + std::abs(cfa[indx + 1] - cfa[indx - 1]) +
                     std::abs(cfa[indx] - cfa[indx + 2]) + std::abs(cfa[indx + 1] - cfa[indx + 3]) +
                     std::abs(cfa[indx + 2] - cfa[indx + 4]);

      // Cardina pixel estimations
      float N_est = cfa[indx - w1] *
                    (1.f + (lpf[indx] - lpf[indx - w2]) / (eps + lpf[indx] + lpf[indx - w2]));
      float S_est = cfa[indx + w1] *
                    (1.f + (lpf[indx] - lpf[indx + w2]) / (eps + lpf[indx] + lpf[indx + w2]));
      float W_est =
          cfa[indx - 1] * (1.f + (lpf[indx] - lpf[indx - 2]) / (eps + lpf[indx] + lpf[indx - 2]));
      float E_est =
          cfa[indx + 1] * (1.f + (lpf[indx] - lpf[indx + 2]) / (eps + lpf[indx] + lpf[indx + 2]));

      // Vertical and horizontal estimations
      float V_est  = (S_grad * N_est + N_grad * S_est) / (N_grad + S_grad);
      float H_est  = (W_grad * E_est + E_grad * W_est) / (E_grad + W_grad);

      // G@B and G@R interpolation
      rgb[indx][1] = std::max(VH_disc * H_est + (1.f - VH_disc) * V_est, 0.f);
    }
  }
  low_pass.release();

  // Step 4: Populate the red and blue channels
  // Step 4.1: Calculate P/Q diagonal local discrimination
#pragma omp parallel for
  for (int row = 4; row < height - 4; row++) {
    for (int col = 4 + (FC(row, 0) & 1); col < width - 4; col += 2) {
      int   indx   = row * width + col;
      float P_stat = std::max(
          -18.f * cfa[indx] * cfa[indx - w1 - 1] - 18.f * cfa[indx] * cfa[indx + w1 + 1] -
              36.f * cfa[indx] * cfa[indx - w2 - 2] - 36.f * cfa[indx] * cfa[indx + w2 + 2] +
              18.f * cfa[indx] * cfa[indx - w3 - 3] + 18.f * cfa[indx] * cfa[indx + w3 + 3] -
              2.f * cfa[indx] * cfa[indx - w4 - 4] - 2.f * cfa[indx] * cfa[indx + w4 + 4] +
              38.f * cfa[indx] * cfa[indx] - 70.f * cfa[indx - w1 - 1] * cfa[indx + w1 + 1] -
              12.f * cfa[indx - w1 - 1] * cfa[indx - w2 - 2] +
              24.f * cfa[indx - w1 - 1] * cfa[indx + w2 + 2] -
              38.f * cfa[indx - w1 - 1] * cfa[indx - w3 - 3] +
              16.f * cfa[indx - w1 - 1] * cfa[indx + w3 + 3] +
              12.f * cfa[indx - w1 - 1] * cfa[indx - w4 - 4] -
              6.f * cfa[indx - w1 - 1] * cfa[indx + w4 + 4] +
              46.f * cfa[indx - w1 - 1] * cfa[indx - w1 - 1] +
              24.f * cfa[indx + w1 + 1] * cfa[indx - w2 - 2] -
              12.f * cfa[indx + w1 + 1] * cfa[indx + w2 + 2] +
              16.f * cfa[indx + w1 + 1] * cfa[indx - w3 - 3] -
              38.f * cfa[indx + w1 + 1] * cfa[indx + w3 + 3] -
              6.f * cfa[indx + w1 + 1] * cfa[indx - w4 - 4] +
              12.f * cfa[indx + w1 + 1] * cfa[indx + w4 + 4] +
              46.f * cfa[indx + w1 + 1] * cfa[indx + w1 + 1] +
              14.f * cfa[indx - w2 - 2] * cfa[indx + w2 + 2] -
              12.f * cfa[indx - w2 - 2] * cfa[indx + w3 + 3] -
              2.f * cfa[indx - w2 - 2] * cfa[indx - w4 - 4] +
              2.f * cfa[indx - w2 - 2] * cfa[indx + w4 + 4] +
              11.f * cfa[indx - w2 - 2] * cfa[indx - w2 - 2] -
              12.f * cfa[indx + w2 + 2] * cfa[indx - w3 - 3] +
              2 * cfa[indx + w2 + 2] * cfa[indx - w4 - 4] -
              2.f * cfa[indx + w2 + 2] * cfa[indx + w4 + 4] +
              11.f * cfa[indx + w2 + 2] * cfa[indx + w2 + 2] +
              2.f * cfa[indx - w3 - 3] * cfa[indx + w3 + 3] -
              6.f * cfa[indx - w3 - 3] * cfa[indx - w4 - 4] +
              10.f * cfa[indx - w3 - 3] * cfa[indx - w3 - 3] -
              6.f * cfa[indx + w3 + 3] * cfa[indx + w4 + 4] +
              10.f * cfa[indx + w3 + 3] * cfa[indx + w3 + 3] +
              1.f * cfa[indx - w4 - 4] * cfa[indx - w4 - 4] +
              1.f * cfa[indx + w4 + 4] * cfa[indx + w4 + 4],
          eps_sq);
      float Q_stat = std::max(
          -18.f * cfa[indx] * cfa[indx + w1 - 1] - 18.f * cfa[indx] * cfa[indx - w1 + 1] -
              36.f * cfa[indx] * cfa[indx + w2 - 2] - 36.f * cfa[indx] * cfa[indx - w2 + 2] +
              18.f * cfa[indx] * cfa[indx + w3 - 3] + 18.f * cfa[indx] * cfa[indx - w3 + 3] -
              2.f * cfa[indx] * cfa[indx + w4 - 4] - 2.f * cfa[indx] * cfa[indx - w4 + 4] +
              38.f * cfa[indx] * cfa[indx] - 70.f * cfa[indx + w1 - 1] * cfa[indx - w1 + 1] -
              12.f * cfa[indx + w1 - 1] * cfa[indx + w2 - 2] +
              24.f * cfa[indx + w1 - 1] * cfa[indx - w2 + 2] -
              38.f * cfa[indx + w1 - 1] * cfa[indx + w3 - 3] +
              16.f * cfa[indx + w1 - 1] * cfa[indx - w3 + 3] +
              12.f * cfa[indx + w1 - 1] * cfa[indx + w4 - 4] -
              6.f * cfa[indx + w1 - 1] * cfa[indx - w4 + 4] +
              46.f * cfa[indx + w1 - 1] * cfa[indx + w1 - 1] +
              24.f * cfa[indx - w1 + 1] * cfa[indx + w2 - 2] -
              12.f * cfa[indx - w1 + 1] * cfa[indx - w2 + 2] +
              16.f * cfa[indx - w1 + 1] * cfa[indx + w3 - 3] -
              38.f * cfa[indx - w1 + 1] * cfa[indx - w3 + 3] -
              6.f * cfa[indx - w1 + 1] * cfa[indx + w4 - 4] +
              12.f * cfa[indx - w1 + 1] * cfa[indx - w4 + 4] +
              46.f * cfa[indx - w1 + 1] * cfa[indx - w1 + 1] +
              14.f * cfa[indx + w2 - 2] * cfa[indx - w2 + 2] -
              12.f * cfa[indx + w2 - 2] * cfa[indx - w3 + 3] -
              2.f * cfa[indx + w2 - 2] * cfa[indx + w4 - 4] +
              2.f * cfa[indx + w2 - 2] * cfa[indx - w4 + 4] +
              11.f * cfa[indx + w2 - 2] * cfa[indx + w2 - 2] -
              12.f * cfa[indx - w2 + 2] * cfa[indx + w3 - 3] +
              2 * cfa[indx - w2 + 2] * cfa[indx + w4 - 4] -
              2.f * cfa[indx - w2 + 2] * cfa[indx - w4 + 4] +
              11.f * cfa[indx - w2 + 2] * cfa[indx - w2 + 2] +
              2.f * cfa[indx + w3 - 3] * cfa[indx - w3 + 3] -
              6.f * cfa[indx + w3 - 3] * cfa[indx + w4 - 4] +
              10.f * cfa[indx + w3 - 3] * cfa[indx + w3 - 3] -
              6.f * cfa[indx - w3 + 3] * cfa[indx - w4 + 4] +
              10.f * cfa[indx - w3 + 3] * cfa[indx - w3 + 3] +
              1.f * cfa[indx + w4 - 4] * cfa[indx + w4 - 4] +
              1.f * cfa[indx - w4 + 4] * cfa[indx - w4 + 4],
          eps_sq);
      PQ_Dir[indx] = P_stat / (P_stat + Q_stat);
    }
  }
  // Step 4.2: Populate the red and blue channels at blue and red CFA positions

#pragma omp parallel for
  for (int row = 4; row < height - 4; row++) {
    for (int col = 4 + (FC(row, 0) & 1); col < width - 4; col += 2) {
      int   indx           = row * width + col;
      int   c              = 2 - FC(row, col);

      // Refined P/Q directional discrimination
      float PQ_central_val = PQ_Dir[indx];
      float PQ_neigh_val   = 0.25f * (PQ_Dir[indx - w1 - 1] + PQ_Dir[indx - w1 + 1] +
                                    PQ_Dir[indx + w1 - 1] + PQ_Dir[indx + w1 + 1]);

      float PQ_disc        = (std::abs(0.5f - PQ_central_val) < std::abs(0.5f - PQ_neigh_val))
                                 ? PQ_neigh_val
                                 : PQ_central_val;

      // Diagonal gradients
      float NW_grad        = eps + std::abs(rgb[indx - w1 - 1][c] - rgb[indx + w1 + 1][c]) +
                      std::abs(rgb[indx - w1 - 1][c] - rgb[indx - w3 - 3][c]) +
                      std::abs(rgb[indx][1] - rgb[indx - w2 - 2][1]);
      float NE_grad = eps + std::abs(rgb[indx - w1 + 1][c] - rgb[indx + w1 - 1][c]) +
                      std::abs(rgb[indx - w1 + 1][c] - rgb[indx - w3 + 3][c]) +
                      std::abs(rgb[indx][1] - rgb[indx - w2 + 2][1]);
      float SW_grad = eps + std::abs(rgb[indx + w1 - 1][c] - rgb[indx - w1 + 1][c]) +
                      std::abs(rgb[indx + w1 - 1][c] - rgb[indx + w3 - 3][c]) +
                      std::abs(rgb[indx][1] - rgb[indx + w2 - 2][1]);
      float SE_grad = eps + std::abs(rgb[indx + w1 + 1][c] - rgb[indx - w1 - 1][c]) +
                      std::abs(rgb[indx + w1 + 1][c] - rgb[indx + w3 + 3][c]) +
                      std::abs(rgb[indx][1] - rgb[indx + w2 + 2][1]);

      // Diagonal color differences
      float NW_est = rgb[indx - w1 - 1][c] - rgb[indx - w1 - 1][1];
      float NE_est = rgb[indx - w1 + 1][c] - rgb[indx - w1 + 1][1];
      float SW_est = rgb[indx + w1 - 1][c] - rgb[indx + w1 - 1][1];
      float SE_est = rgb[indx + w1 + 1][c] - rgb[indx + w1 + 1][1];

      // P/Q estimations
      float P_est  = (NW_grad * SE_est + SE_grad * NW_est) / (NW_grad + SE_grad);
      float Q_est  = (NE_grad * SW_est + SW_grad * NE_est) / (NE_grad + SW_grad);

      // R@B and B@R interpolation
      rgb[indx][c] = std::max(0.f, rgb[indx][1] + (1.f - PQ_disc) * P_est + PQ_disc * Q_est);
    }
  }

  PQ_dir.release();

  // Step 4.3 Populate the red and blue channels at green CFA positions
#pragma omp parallel for
  for (int row = 4; row < height - 4; row++) {
    for (int col = 4 + (FC(row, 1) & 1); col < width - 4; col += 2) {
      int   indx           = row * width + col;
      float VH_central_val = VH_Dir[indx];
      float VH_neigh_val   = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] +
                                    VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
      float VH_disc        = (std::abs(0.5f - VH_central_val) < std::abs(0.5f - VH_neigh_val))
                                 ? VH_neigh_val
                                 : VH_central_val;

      for (int c = 0; c <= 2; c += 2) {
        // Cardinal gradients
        float N_grad = eps + std::abs(rgb[indx][1] - rgb[indx - w2][1]) +
                       std::abs(rgb[indx - w1][c] - rgb[indx + w1][c]) +
                       std::abs(rgb[indx - w1][c] - rgb[indx - w3][c]);
        float S_grad = eps + std::abs(rgb[indx][1] - rgb[indx + w2][1]) +
                       std::abs(rgb[indx + w1][c] - rgb[indx - w1][c]) +
                       std::abs(rgb[indx + w1][c] - rgb[indx + w3][c]);
        float W_grad = eps + std::abs(rgb[indx][1] - rgb[indx - 2][1]) +
                       std::abs(rgb[indx - 1][c] - rgb[indx + 1][c]) +
                       std::abs(rgb[indx - 1][c] - rgb[indx - 3][c]);
        float E_grad = eps + std::abs(rgb[indx][1] - rgb[indx + 2][1]) +
                       std::abs(rgb[indx + 1][c] - rgb[indx - 1][c]) +
                       std::abs(rgb[indx + 1][c] - rgb[indx + 3][c]);

        // Cardinal colour differences
        float N_est  = rgb[indx - w1][c] - rgb[indx - w1][1];
        float S_est  = rgb[indx + w1][c] - rgb[indx + w1][1];
        float W_est  = rgb[indx - 1][c] - rgb[indx - 1][1];
        float E_est  = rgb[indx + 1][c] - rgb[indx + 1][1];

        // Vertical and horizontal estimations
        float V_est  = (N_grad * S_est + S_grad * N_est) / (N_grad + S_grad);
        float H_est  = (E_grad * W_est + W_grad * E_est) / (E_grad + W_grad);

        // R@G and B@G interpolation
        rgb[indx][c] = std::max(0.f, rgb[indx][1] + (1.f - VH_disc) * V_est + VH_disc * H_est);
      }
    }
  }
  VH_dir.release();
  // delete[] rgb;

  output.copyTo(bayer);
  bayer.convertTo(bayer, CV_32FC3);
}
}  // namespace CPU
}  // namespace alcedo