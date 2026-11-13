//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
//  This file also contains material subject to the upstream notices below.

/*
    Copyright (C) 2022 darktable developers

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/* darktable:
   The refavg values are calculated in raw-RGB-cube3 space
   We calculate all color channels in the 3x3 photosite area, this can be understaood as a
   "superpixel", the "asking" location is in the centre. As this works for bayer and xtrans sensors
   we don't have a fixed ratio but calculate the average for every color channel first. refavg for
   one of red, green or blue is defined as means of both other color channels (opposing).

   The basic idea / observation for the _process_opposed algorithm is, the refavg is a good estimate
   for any clipped color channel in the vast majority of images, working mostly fine both for small
   specular highlighted spots and large areas.

   The correction via some sort of global chrominance further helps to correct color casts.
   The chrominace data are taken from the areas morphologically very close to clipped data.
   Failures of the algorithm (color casts) are in most cases related to
    a) very large differences between optimal white balance coefficients vs what we have as D65 in
   the darktable pipeline b) complicated lightings so the gradients are not well related c) a wrong
   whitepoint setting in the rawprepare module. d) the maths might not be best

   Alcedo Studio changes:
   - Adapted to work with libraw and modified desaturation strategy.
   - Add a new strategy to desaturate restored highlights based on the number of clipped channels [DEPRECATED].
*/

#include "decoders/processor/operators/cpu/highlight_reconstruct.hpp"

#include <libraw/libraw_types.h>
#include <opencv2/core/hal/interface.h>

#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#define HL_POWERF 3.0f

static int fc[2][2] = {{0, 1}, {2, 1}};  // R=0, G1=1, B=2, G2=1
#ifndef FC
#define FC(y, x) fc[(y) & 1][(x) & 1]
#endif

namespace alcedo {
namespace CPU {

int round_size(const int size, const int alignment) {
  // Round the size of a buffer to the closest higher multiple
  return ((size % alignment) == 0) ? size : ((size - 1) / alignment + 1) * alignment;
}

static inline char mask_dilate(const unsigned char* in, const int w1) {
  if (in[0]) return 1;

  if (in[-w1 - 1] | in[-w1] | in[-w1 + 1] | in[-1] | in[1] | in[w1 - 1] | in[w1] | in[w1 + 1])
    return 1;

  const int w2 = 2 * w1;
  const int w3 = 3 * w1;
  return (in[-w3 - 2] | in[-w3 - 1] | in[-w3] | in[-w3 + 1] | in[-w3 + 2] | in[-w2 - 3] |
          in[-w2 - 2] | in[-w2 - 1] | in[-w2] | in[-w2 + 1] | in[-w2 + 2] | in[-w2 + 3] |
          in[-w1 - 3] | in[-w1 - 2] | in[-w1 + 2] | in[-w1 + 3] | in[-3] | in[-2] | in[2] | in[3] |
          in[w1 - 3] | in[w1 - 2] | in[w1 + 2] | in[w1 + 3] | in[w2 - 3] | in[w2 - 2] | in[w2 - 1] |
          in[w2] | in[w2 + 1] | in[w2 + 2] | in[w2 + 3] | in[w3 - 2] | in[w3 - 1] | in[w3] |
          in[w3 + 1] | in[w3 + 2])
             ? 1
             : 0;
}

static inline int _raw_to_cmap(const int width, const int row, const int col) {
  return (row / 3) * width + (col / 3);
}

static inline float _calc_refavg(const float* in, const int row, const int col, const int height,
                                 const int width, float* correction) {
  const int color   = FC(row, col);
  float     mean[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float     cnt[4]  = {0.0f, 0.0f, 0.0f, 0.0f};

  const int dymin   = std::max(0, row - 1);
  const int dxmin   = std::max(0, col - 1);
  const int dymax   = std::min(height - 1, row + 2);
  const int dxmax   = std::min(width - 1, col + 2);

  for (int dy = dymin; dy < dymax; ++dy) {
    for (int dx = dxmin; dx < dxmax; ++dx) {
      const float val = fmaxf(0.0f, in[dy * width + dx]);
      const int   c   = FC(dy, dx);
      mean[c] += val;
      cnt[c] += 1.0f;
    }
  }
  for (int c = 0; c < 3; ++c) {
    mean[c] = (cnt[c] > 0.f) ? powf(correction[c] * mean[c] / cnt[c], 1.0f / HL_POWERF) : 0.f;
  }

  const float croot_refavg[4] = {0.5f * (mean[1] + mean[2]), 0.5f * (mean[0] + mean[2]),
                                 0.5f * (mean[0] + mean[1]), 0.0f};
  return powf(croot_refavg[color], HL_POWERF);
  // return croot_refavg[color];
}

/**
 * @brief Adapted from
 * https://github.com/darktable-org/darktable/blob/master/src/iop/hlreconstruct/opposed.c
 *
 * @param img
 * @param raw_processor
 */
void HighlightReconstruct(cv::Mat& img, LibRaw& raw_processor) {
  const int          width         = img.cols;
  const int          height        = img.rows;

  static const float hilight_magic = 0.987f;  // default value from darktable
  // float max_val = static_cast<float>(raw_processor.imgdata.rawdata.color.maximum) / 65535.0f;

  auto               cam_mul       = raw_processor.imgdata.color.cam_mul;

  float              correction[4] = {cam_mul[0] / cam_mul[1], 1.f, cam_mul[2] / cam_mul[1], 0.f};

  const float        clip_val      = hilight_magic;

  const float        clips[3]      = {clip_val, clip_val, clip_val};

  // Didn't know why darktable use m_width and m_height
  const int          m_width       = width / 3;
  const int          m_height      = height / 3;
  const int          m_size        = round_size((int)(m_width + 1) * (m_height + 1), 16);

  bool               anyclipped    = false;
  cv::Mat1f          input(img);

  auto               input_data = input.ptr<float>(0);

  std::vector<unsigned char> mask_buf(6 * m_size, 0);

  for (int row = 1; row < m_height - 1; ++row) {
    for (int col = 1; col < m_width - 1; ++col) {
      char      mbuff[3] = {0, 0, 0};
      const int grp      = 3 * (row * width + col);
      for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
          const int  idx     = grp + y * width + x;
          const int  color   = FC(row + y, col + x);
          const char clipped = input_data[idx] >= clips[color] ? 1 : 0;
          mbuff[color] += (clipped) ? 1 : 0;
        }
      }
      // const int cmx = _raw_to_cmap(m_width, row, col);
      for (int c = 0; c < 3; ++c) {
        if (mbuff[c]) {
          mask_buf[c * m_size + row * m_width + col] = 1;
          anyclipped                                 = true;
        }
      }
    }
  }

  // DebuggingPreview(mask);

  /* We want to use the photosites closely around clipped data to be taken into account.
     The mask buffers holds data for each color channel, we dilate the mask buffer slightly
     to get those locations.
     If there are no clipped locations we keep the chrominance correction at 0 but make it valid
  */

  float sums[4] = {0.f, 0.f, 0.f, 0.f};
  float cnts[4] = {0.f, 0.f, 0.f, 0.f};

  if (anyclipped) {
    for (int row = 3; row < static_cast<int>(m_height) - 3; ++row) {
      for (int col = 3; col < static_cast<int>(m_width) - 3; ++col) {
        const int mx              = static_cast<int>(row) * m_width + static_cast<int>(col);
        mask_buf[3 * m_size + mx] = mask_dilate(mask_buf.data() + 0 * m_size + mx, m_width);
        mask_buf[4 * m_size + mx] = mask_dilate(mask_buf.data() + 1 * m_size + mx, m_width);
        mask_buf[5 * m_size + mx] = mask_dilate(mask_buf.data() + 2 * m_size + mx, m_width);
      }
    }

    const float lo_clips[4] = {0.99f * clips[0], 0.99f * clips[1], 0.99f * clips[2], 1.0f};
    /* After having the surrounding mask for each color channel we can calculate the chrominance
     * corrections. */

    for (int row = 3; row < height - 3; ++row) {
      for (int col = 3; col < width - 3; ++col) {
        const int   color = FC(row, col);
        const float inval = input(row, col);

        /* we only use the unclipped photosites very close the true clipped data to calculate the
         * chrominance offset */
        if ((inval < clips[color]) && (inval > lo_clips[color]) &&
            (mask_buf[(color + 3) * m_size + _raw_to_cmap(m_width, row, col)])) {
          sums[color] +=
              inval - _calc_refavg(input.ptr<float>(0), row, col, height, width, correction);
          cnts[color] += 1.0f;
        }
      }
    }

    float chrominance[4] = {0.f, 0.f, 0.f, 0.f};
    for (int c = 0; c < 3; ++c) {
      chrominance[c] = (cnts[c] > 80.f) ? (sums[c] / cnts[c]) : 0.f;
    }

    // std::cout << "Correction: R=" << correction[0] << " G=" << correction[1]
    //           << " B=" << correction[2] << std::endl;
    // std::cout << "Chrominance: R=" << chrominance[0] << " G=" << chrominance[1]
    //           << " B=" << chrominance[2] << std::endl;
    cv::Mat1f result = input.clone();

    for (int row = 0; row < height; ++row) {
      for (int col = 0; col < width; ++col) {
        const int   color = FC(row, col);
        const float inval = MAX(0.0f, input(row, col));
        if (inval >= clips[color]) {
          const float ref  = _calc_refavg(input.ptr<float>(0), row, col, height, width, correction);
          result(row, col) = std::max(inval, ref + chrominance[color]);
        } else {
          result(row, col) = inval;
        }
      }
    }

    // cv::Mat1f final_result = result.clone();

    // for (int row = 2; row < height - 2; ++row) {
    //   for (int col = 2; col < width - 2; ++col) {
    //     const int   color = FC(row, col);
    //     const float inval = input(row, col);

    //     // Only process clipped pixels
    //     if (inval >= clips[color]) {
    //       const float reconstructed = result(row, col);

    //       // Calculate how many channels are clipped in neighborhood
    //       float       desat_factor =
    //           _calc_desaturation_factor(input.ptr<float>(0), row, col, height, width, clips);

    //       if (desat_factor > 0.0f) {
    //         // Calculate local luminance estimate from surrounding pixels
    //         float lum_sum = 0.0f;
    //         float lum_cnt = 0.0f;

    //         for (int dy = -2; dy <= 2; ++dy) {
    //           for (int dx = -2; dx <= 2; ++dx) {
    //             const int ny = row + dy;
    //             const int nx = col + dx;
    //             if (ny >= 0 && ny < height && nx >= 0 && nx < width) {
    //               lum_sum += result(ny, nx);
    //               lum_cnt += 1.0f;
    //             }
    //           }
    //         }

    //         const float local_lum = (lum_cnt > 0.0f) ? (lum_sum / lum_cnt) : reconstructed;

    //         // Blend towards neutral (luminance) based on desaturation factor
    //         // This effectively reduces saturation while preserving brightness
    //         final_result(row, col) =
    //             reconstructed * (1.0f - desat_factor) + local_lum * desat_factor;
    //       } else {
    //         final_result(row, col) = reconstructed;
    //       }
    //     } else {
    //       final_result(row, col) = result(row, col);
    //     }
    //   }
    // }

    img = result;
  }
}
};  // namespace CPU
};  // namespace alcedo