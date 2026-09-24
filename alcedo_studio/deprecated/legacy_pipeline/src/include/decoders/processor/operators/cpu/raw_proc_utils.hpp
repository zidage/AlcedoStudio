//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <ostream>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "hwy/highway.h"

namespace alcedo {
namespace CPU {
enum class LightSourceType : uint8_t {
  UNKNOWN      = 0,
  DAYLIGHT     = 1,
  FLUORESCENT  = 2,
  TUNGSTEN     = 3,
  FLASH        = 4,
  FINE_WEATHER = 9,
  CLOUDY       = 10,
  SHADE        = 11,
  STD_A        = 17,
  D55          = 20,
  D65          = 21,
  D75          = 22,
  D50          = 23
};

const int   COLOR_TEMP_TABLE_SIZE              = 24;
const float EXIF_WB_MAP[COLOR_TEMP_TABLE_SIZE] = {
    0.0f, 5500.0f, 4200.0f, 2850.0f, 5500.0f, 5200.0f, 0.0f,    0.0f,
    0.0f, 5200.0f, 6500.0f, 7500.0f, 0.0f,    0.0f,    0.0f,    0.0f,
    0.0f, 2850.0f, 0.0f,    0.0f,    5500.0f, 6500.0f, 7500.0f, 5000.0f};

inline static float GetTempForWBIndices(int idx) {
  if (idx < 0 || idx >= COLOR_TEMP_TABLE_SIZE) {
    return -1.0f;
  }
  return EXIF_WB_MAP[idx];
}

/**
 * @brief Get the WB Indices For Temp object. If exact match is found, both indices will be the
 * same. Otherwise, the lower and upper indices surrounding the temperature are returned.
 *
 * @param temp
 * @return std::pair<int, int>
 */
inline static std::pair<int, int> GetWBIndicesForTemp(float temp) {
  std::pair<int, int> result{-1, -1};  // lower index, upper index
  float               lower_tmp = -1.0f;
  float               upper_tmp = -1.0f;
  for (int i = 1; i < COLOR_TEMP_TABLE_SIZE; ++i) {
    if (EXIF_WB_MAP[i] == 0.0f) continue;
    if (EXIF_WB_MAP[i] <= temp) {
      if (EXIF_WB_MAP[i] == temp) {
        result.first  = i;
        result.second = i;
        // Exact match
        return result;
      }
      if (EXIF_WB_MAP[i] > lower_tmp) {
        lower_tmp    = EXIF_WB_MAP[i];
        result.first = i;
      }
    } else {
      if (upper_tmp < 0.0f || EXIF_WB_MAP[i] < upper_tmp) {
        upper_tmp     = EXIF_WB_MAP[i];
        result.second = i;
      }
    }
  }
  return result;
}

struct ChannelStats {
  float max_    = 0.0f;
  float median_ = 0.0f;
  float mean_   = 0.0f;
};

/**
 * @brief Compute per-channel max/median/mean statistics.
 *
 * - Supports 1/3/4 channel images (and generally any channel count > 0).
 * - If the input is not float, it is converted to CV_32F internally.
 * - NaN/Inf values are ignored; if a channel has no finite samples, stats are 0.
 */
inline static std::vector<ChannelStats> ComputeChannelStats(const cv::Mat& img) {
  CV_Assert(!img.empty());
  CV_Assert(img.channels() > 0);

  cv::Mat float_img;
  if (img.depth() != CV_32F) {
    img.convertTo(float_img, CV_MAKETYPE(CV_32F, img.channels()));
  } else {
    float_img = img;
  }

  const int channels = float_img.channels();
  std::vector<ChannelStats> out(static_cast<size_t>(channels));

  const size_t total_pixels = static_cast<size_t>(float_img.total());
  if (total_pixels == 0) return out;

  std::vector<std::vector<float>> samples(static_cast<size_t>(channels));
  for (int c = 0; c < channels; ++c) {
    samples[static_cast<size_t>(c)].reserve(total_pixels);
    out[static_cast<size_t>(c)].max_ = -std::numeric_limits<float>::infinity();
  }

  std::vector<double> sums(static_cast<size_t>(channels), 0.0);
  std::vector<size_t> counts(static_cast<size_t>(channels), 0);

  const int rows = float_img.rows;
  const int cols = float_img.cols;
  if (float_img.isContinuous()) {
    const float* base = float_img.ptr<float>(0);
    const size_t stride = static_cast<size_t>(channels);
    for (size_t i = 0; i < total_pixels; ++i) {
      const float* px = base + i * stride;
      for (int c = 0; c < channels; ++c) {
        float v = px[c];
        if (!std::isfinite(v)) continue;
        auto& vec = samples[static_cast<size_t>(c)];
        vec.push_back(v);
        sums[static_cast<size_t>(c)] += static_cast<double>(v);
        counts[static_cast<size_t>(c)] += 1;
        out[static_cast<size_t>(c)].max_ = fmax(out[static_cast<size_t>(c)].max_, v);
      }
    }
  } else {
    for (int r = 0; r < rows; ++r) {
      const float* row_ptr = float_img.ptr<float>(r);
      for (int x = 0; x < cols; ++x) {
        const float* px = row_ptr + static_cast<size_t>(x) * static_cast<size_t>(channels);
        for (int c = 0; c < channels; ++c) {
          float v = px[c];
          if (!std::isfinite(v)) continue;
          auto& vec = samples[static_cast<size_t>(c)];
          vec.push_back(v);
          sums[static_cast<size_t>(c)] += static_cast<double>(v);
          counts[static_cast<size_t>(c)] += 1;
          out[static_cast<size_t>(c)].max_ = fmax(out[static_cast<size_t>(c)].max_, v);
        }
      }
    }
  }

  for (int c = 0; c < channels; ++c) {
    const size_t idx = static_cast<size_t>(c);
    const size_t n   = counts[idx];
    if (n == 0) {
      out[idx] = {};
      continue;
    }

    out[idx].mean_ = static_cast<float>(sums[idx] / static_cast<double>(n));

    auto& vec = samples[idx];
    const size_t k = n / 2;
    auto mid_it    = vec.begin() + static_cast<std::ptrdiff_t>(k);
    std::nth_element(vec.begin(), mid_it, vec.end());
    const float m1 = *mid_it;

    if ((n & 1U) == 1U) {
      out[idx].median_ = m1;
    } else {
      auto mid2_it = vec.begin() + static_cast<std::ptrdiff_t>(k - 1);
      std::nth_element(vec.begin(), mid2_it, vec.end());
      const float m0 = *mid2_it;
      out[idx].median_ = 0.5f * (m0 + m1);
    }
  }

  return out;
}

inline static void PrintChannelStats(const std::vector<ChannelStats>& stats, std::ostream& os) {
  os << std::fixed << std::setprecision(6);
  for (size_t c = 0; c < stats.size(); ++c) {
    os << "ch" << c << ": max=" << stats[c].max_ << ", median=" << stats[c].median_
       << ", mean=" << stats[c].mean_ << "\n";
  }
}

inline static void PrintChannelStats(const cv::Mat& img, std::ostream& os) {
  PrintChannelStats(ComputeChannelStats(img), os);
}

void boxblur2(const cv::Mat1f& src, cv::Mat1f& dst, cv::Mat1f& temp, int startY, int startX, int H,
              int W, int box);

void boxblur_resamp(const cv::Mat1f& src, cv::Mat1f& dst, cv::Mat1f& temp, int H, int W, int box,
                    int samp);

inline static void DebuggingPreview(cv::Mat& src) {
  cv::Mat resized;
  cv::resize(src, resized, cv::Size(512, 512));
  cv::imshow("Debugging Preview", resized);
  cv::waitKey(0);
}
};  // namespace CPU
};  // namespace alcedo