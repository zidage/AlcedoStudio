//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <opencv2/core/mat.hpp>
#include <stdexcept>
#include <string>

namespace alcedo {

#if defined(__CUDACC__)
#define ALCEDO_HD __host__ __device__
#else
#define ALCEDO_HD
#endif

enum class RawInputKind {
  BayerRaw,
  DebayeredRgb,
  Unsupported,
  /// JPEG, PNG, TIFF or OpenEXR pixels decoded by RasterInputLoader. Display-referred or
  /// scene-linear source RGB; the Develop `input` object describes its color.
  RasterRgb,
};

enum class RawCfaKind {
  Bayer2x2,
  XTrans6x6,
};

struct BayerPattern2x2 {
  int raw_fc[4] = {0, 1, 3, 2};
  int rgb_fc[4] = {0, 1, 1, 2};
};

struct XTransPattern6x6 {
  int raw_fc[36] = {};
  int rgb_fc[36] = {};
};

struct RawCfaPattern {
  RawCfaKind     kind          = RawCfaKind::Bayer2x2;
  BayerPattern2x2 bayer_pattern = {};
  XTransPattern6x6 xtrans_pattern = {};
};

ALCEDO_HD inline auto BayerCellIndex(const int y, const int x) -> int {
  return ((y & 1) << 1) | (x & 1);
}

ALCEDO_HD inline auto WrapPatternCoord(const int coord, const int period) -> int {
  const int wrapped = coord % period;
  return wrapped < 0 ? wrapped + period : wrapped;
}

ALCEDO_HD inline auto XTransCellIndex(const int y, const int x) -> int {
  return WrapPatternCoord(y, 6) * 6 + WrapPatternCoord(x, 6);
}

ALCEDO_HD inline auto FoldRawColorToRgb(const int raw_color) -> int {
  switch (raw_color) {
    case 0:
      return 0;
    case 2:
      return 2;
    case 1:
    case 3:
      return 1;
    default:
      return raw_color;
  }
}

ALCEDO_HD inline auto RawColorAt(const BayerPattern2x2& pattern, const int y, const int x)
    -> int {
  return pattern.raw_fc[BayerCellIndex(y, x)];
}

ALCEDO_HD inline auto RgbColorAt(const BayerPattern2x2& pattern, const int y, const int x)
    -> int {
  return pattern.rgb_fc[BayerCellIndex(y, x)];
}

ALCEDO_HD inline auto RawColorAt(const XTransPattern6x6& pattern, const int y, const int x)
    -> int {
  return pattern.raw_fc[XTransCellIndex(y, x)];
}

ALCEDO_HD inline auto RgbColorAt(const XTransPattern6x6& pattern, const int y, const int x)
    -> int {
  return pattern.rgb_fc[XTransCellIndex(y, x)];
}

ALCEDO_HD inline auto RawColorAt(const RawCfaPattern& pattern, const int y, const int x)
    -> int {
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    return RawColorAt(pattern.xtrans_pattern, y, x);
  }
  return RawColorAt(pattern.bayer_pattern, y, x);
}

ALCEDO_HD inline auto RgbColorAt(const RawCfaPattern& pattern, const int y, const int x)
    -> int {
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    return RgbColorAt(pattern.xtrans_pattern, y, x);
  }
  return RgbColorAt(pattern.bayer_pattern, y, x);
}

inline auto DescribeRgbColor(const int color) -> char {
  switch (color) {
    case 0:
      return 'R';
    case 1:
      return 'G';
    case 2:
      return 'B';
    default:
      return '?';
  }
}

inline auto DescribeBayerPattern(const BayerPattern2x2& pattern) -> std::string {
  std::string desc(4, '?');
  for (int i = 0; i < 4; ++i) {
    desc[i] = DescribeRgbColor(pattern.rgb_fc[i]);
  }
  return desc;
}

inline auto DescribeRawCfaPattern(const RawCfaPattern& pattern) -> std::string {
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    return "X-Trans";
  }
  return DescribeBayerPattern(pattern.bayer_pattern);
}

inline auto IsClassic2x2Bayer(const BayerPattern2x2& pattern) -> bool {
  int raw_sorted[4] = {pattern.raw_fc[0], pattern.raw_fc[1], pattern.raw_fc[2], pattern.raw_fc[3]};
  std::sort(std::begin(raw_sorted), std::end(raw_sorted));

  int rgb_sorted[4] = {pattern.rgb_fc[0], pattern.rgb_fc[1], pattern.rgb_fc[2], pattern.rgb_fc[3]};
  std::sort(std::begin(rgb_sorted), std::end(rgb_sorted));

  static constexpr int kExpectedRaw[4] = {0, 1, 2, 3};
  static constexpr int kExpectedRgb[4] = {0, 1, 1, 2};

  return std::equal(std::begin(raw_sorted), std::end(raw_sorted), std::begin(kExpectedRaw)) &&
         std::equal(std::begin(rgb_sorted), std::end(rgb_sorted), std::begin(kExpectedRgb));
}

inline auto IsRGGBPattern(const BayerPattern2x2& pattern) -> bool {
  static constexpr int kRggbRaw[4] = {0, 1, 3, 2};
  return std::equal(std::begin(pattern.raw_fc), std::end(pattern.raw_fc), std::begin(kRggbRaw));
}

inline auto IsXTransPattern(const RawCfaPattern& pattern) -> bool {
  return pattern.kind == RawCfaKind::XTrans6x6;
}

inline auto ReadLibRawBayerPattern(LibRaw& raw_processor) -> BayerPattern2x2 {
  BayerPattern2x2 pattern = {};
  for (int row = 0; row < 2; ++row) {
    for (int col = 0; col < 2; ++col) {
      const int index       = BayerCellIndex(row, col);
      pattern.raw_fc[index] = raw_processor.COLOR(row, col);
      pattern.rgb_fc[index] = FoldRawColorToRgb(pattern.raw_fc[index]);
    }
  }
  return pattern;
}

inline auto ReadLibRawXTransPattern(LibRaw& raw_processor) -> XTransPattern6x6 {
  XTransPattern6x6 pattern = {};
  for (int row = 0; row < 6; ++row) {
    for (int col = 0; col < 6; ++col) {
      const int index       = XTransCellIndex(row, col);
      pattern.raw_fc[index] = raw_processor.imgdata.idata.xtrans[row][col];
      pattern.rgb_fc[index] = FoldRawColorToRgb(pattern.raw_fc[index]);
    }
  }
  return pattern;
}

inline auto ReadLibRawCfaPattern(LibRaw& raw_processor) -> RawCfaPattern {
  RawCfaPattern pattern = {};
  if (raw_processor.imgdata.idata.filters == 9U) {
    pattern.kind           = RawCfaKind::XTrans6x6;
    pattern.xtrans_pattern = ReadLibRawXTransPattern(raw_processor);
    return pattern;
  }

  pattern.kind          = RawCfaKind::Bayer2x2;
  pattern.bayer_pattern = ReadLibRawBayerPattern(raw_processor);
  return pattern;
}

inline auto ClassifyRawInput(const libraw_rawdata_t& raw_data, const libraw_iparams_t& idata)
    -> RawInputKind {
  if (raw_data.color3_image != nullptr || raw_data.float3_image != nullptr) {
    return RawInputKind::DebayeredRgb;
  }
  if ((raw_data.color4_image != nullptr || raw_data.float4_image != nullptr) && idata.colors == 3) {
    return RawInputKind::DebayeredRgb;
  }
  if (raw_data.raw_image != nullptr) {
    return RawInputKind::BayerRaw;
  }
  return RawInputKind::Unsupported;
}

template <typename T>
auto DownsampleBayer2xTyped(const cv::Mat& src, const BayerPattern2x2& pattern) -> cv::Mat {
  const int out_rows = src.rows / 2;
  const int out_cols = src.cols / 2;
  cv::Mat   dst(out_rows, out_cols, src.type());

  std::array<int, 4> src_index_for_dst = {-1, -1, -1, -1};
  for (int dst_index = 0; dst_index < 4; ++dst_index) {
    const int expected_color = pattern.raw_fc[dst_index];
    int       src_index      = -1;
    for (int i = 0; i < 4; ++i) {
      if (pattern.raw_fc[i] == expected_color) {
        if (src_index >= 0) {
          throw std::runtime_error("RawProcessor: duplicated Bayer color index in pattern");
        }
        src_index = i;
      }
    }
    if (src_index < 0) {
      throw std::runtime_error("RawProcessor: invalid Bayer pattern for downsample");
    }
    src_index_for_dst[dst_index] = src_index;
  }

  std::array<int, 4> src_row_delta = {};
  std::array<int, 4> src_col_delta = {};
  for (int i = 0; i < 4; ++i) {
    src_row_delta[i] = src_index_for_dst[i] >> 1;
    src_col_delta[i] = src_index_for_dst[i] & 1;
  }

  for (int y = 0; y < out_rows; ++y) {
    T*       drow         = dst.ptr<T>(y);
    const int dst_even_idx = BayerCellIndex(y, 0);
    const int dst_odd_idx  = BayerCellIndex(y, 1);
    const T*  src_even     = src.ptr<T>(2 * y + src_row_delta[dst_even_idx]);
    const T*  src_odd      = src.ptr<T>(2 * y + src_row_delta[dst_odd_idx]);

    int x = 0;
    for (; x + 1 < out_cols; x += 2) {
      const int src_col_even = 2 * x + src_col_delta[dst_even_idx];
      const int src_col_odd  = 2 * (x + 1) + src_col_delta[dst_odd_idx];
      drow[x]                = src_even[src_col_even];
      drow[x + 1]            = src_odd[src_col_odd];
    }
    if (x < out_cols) {
      const int src_col_even = 2 * x + src_col_delta[dst_even_idx];
      drow[x]                = src_even[src_col_even];
    }
  }

  return dst;
}

inline auto DownsampleBayer2x(const cv::Mat& src, const BayerPattern2x2& pattern) -> cv::Mat {
  switch (src.type()) {
    case CV_32FC1:
      return DownsampleBayer2xTyped<float>(src, pattern);
    case CV_16UC1:
      return DownsampleBayer2xTyped<uint16_t>(src, pattern);
    default:
      throw std::runtime_error("RawProcessor: unsupported Bayer type for downsample");
  }
}

template <typename T>
auto DownsampleXTrans2xTyped(const cv::Mat& src, XTransPattern6x6& pattern) -> cv::Mat {
  const int out_rows = src.rows / 2;
  const int out_cols = src.cols / 2;
  cv::Mat   dst(out_rows, out_cols, src.type());

  for (int y = 0; y < out_rows; ++y) {
    T* drow = dst.ptr<T>(y);
    const T* srow = src.ptr<T>(2 * y);
    for (int x = 0; x < out_cols; ++x) {
      drow[x] = srow[2 * x];
    }
  }

  XTransPattern6x6 sampled_pattern = {};
  for (int row = 0; row < 6; ++row) {
    for (int col = 0; col < 6; ++col) {
      const int index                = XTransCellIndex(row, col);
      sampled_pattern.raw_fc[index]  = RawColorAt(pattern, 2 * row, 2 * col);
      sampled_pattern.rgb_fc[index]  = FoldRawColorToRgb(sampled_pattern.raw_fc[index]);
    }
  }
  pattern = sampled_pattern;
  return dst;
}

inline auto DownsampleXTrans2x(const cv::Mat& src, XTransPattern6x6& pattern) -> cv::Mat {
  switch (src.type()) {
    case CV_32FC1:
      return DownsampleXTrans2xTyped<float>(src, pattern);
    case CV_16UC1:
      return DownsampleXTrans2xTyped<uint16_t>(src, pattern);
    default:
      throw std::runtime_error("RawProcessor: unsupported X-Trans type for downsample");
  }
}

inline auto DownsampleRaw2x(const cv::Mat& src, RawCfaPattern& pattern) -> cv::Mat {
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    return DownsampleXTrans2x(src, pattern.xtrans_pattern);
  }
  return DownsampleBayer2x(src, pattern.bayer_pattern);
}

}  // namespace alcedo

#undef ALCEDO_HD
