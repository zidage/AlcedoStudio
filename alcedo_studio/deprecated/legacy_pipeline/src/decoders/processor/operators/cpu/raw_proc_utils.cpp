//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef RAW_PROC_UTILS_CPP
#define RAW_PROC_UTILS_CPP

#include "decoders/processor/operators/cpu/raw_proc_utils.hpp"

#include <opencv2/core.hpp>
#include <vector>

#include "hwy/highway.h"

using namespace hwy::HWY_NAMESPACE;

namespace alcedo {
namespace CPU {
void boxblur2(const cv::Mat1f& src, cv::Mat1f& dst, cv::Mat1f& temp, int startY, int startX, int H,
              int W, int box) {
  CV_Assert(src.type() == CV_32F && dst.type() == CV_32F && temp.type() == CV_32F);
  CV_Assert(src.rows >= startY + H && src.cols >= startX + W);
  CV_Assert(temp.rows == H && temp.cols == W);
  CV_Assert(dst.rows == H && dst.cols == W);

  // ---------- horizontal blur: write into temp ----------
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int row = 0; row < H; ++row) {
    const float* srcRow  = src.ptr<float>(row + startY);
    float*       tempRow = temp.ptr<float>(row);

    int          len     = box + 1;
    float        s       = srcRow[startX] / (float)len;
    for (int j = 1; j <= box; ++j) s += srcRow[startX + j] / (float)len;
    tempRow[0] = s;

    for (int col = 1; col <= box; ++col) {
      float prev   = tempRow[col - 1];
      float addv   = srcRow[startX + col + box];
      tempRow[col] = (prev * (float)len + addv) / (float)(len + 1);
      len++;
    }

    for (int col = box + 1; col < W - box; ++col) {
      float prev   = tempRow[col - 1];
      float addv   = srcRow[startX + col + box];
      float subv   = srcRow[startX + col - box - 1];
      tempRow[col] = prev + (addv - subv) / (float)len;
    }

    for (int col = W - box; col < W; ++col) {
      float prev   = tempRow[col - 1];
      float subv   = srcRow[startX + col - box - 1];
      tempRow[col] = (prev * (float)len - subv) / (float)(len - 1);
      len--;
    }
  }

  // ---------- vertical blur: temp -> dst, use Highway for vectorized inner loops ----------
  // We'll operate column-wise; use Highway vector width determined at compile/run time.
  {
    // Full lane descriptor for float
    const auto   d = HWY_FULL(float)();  // lane descriptor (Full for target)
    const size_t L = Lanes(d);
#ifdef _OPENMP
#pragma omp parallel
#endif
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for (int col = 0; col < (int)(W / (int)L) * (int)L; col += (int)L) {
        float                   lenf = (float)(box + 1);
        auto                    lenv = Set(d, lenf);
        auto                    onev = Set(d, 1.0f);

        Vec<ScalableTag<float>> acc  = Set(ScalableTag<float>(), 0.0f);
        for (int i = 0; i <= box; ++i) {
          const float* tptr = temp.ptr<float>(i) + col;
          auto         v    = Load(d, tptr);
          acc               = acc + v;
        }
        // divide by len
        auto outv = acc / Set(d, lenf);
        Store(outv, d, dst.ptr<float>(0) + col);

        // 增长阶段 row = 1 .. box
        float cur_len = lenf;
        for (int row = 1; row <= box; ++row) {
          // out[row] = (out[row-1]*cur_len + temp[row+box]) / (cur_len + 1)
          const float* addptr    = temp.ptr<float>(row + box) + col;
          auto         addv      = Load(d, addptr);
          auto         prevv     = Load(d, dst.ptr<float>(row - 1) + col);
          auto         numerator = prevv * Set(d, cur_len) + addv;
          auto         denom     = Set(d, cur_len + 1.0f);
          auto         resv      = numerator / denom;
          Store(resv, d, dst.ptr<float>(row) + col);
          cur_len += 1.0f;
        }

        // 中间稳态 row = box+1 .. H-box-1
        for (int row = box + 1; row < H - box; ++row) {
          // out[row] = out[row-1] + (temp[row+box] - temp[row-box-1]) / cur_len
          auto prevv = Load(d, dst.ptr<float>(row - 1) + col);
          auto addv  = Load(d, temp.ptr<float>(row + box) + col);
          auto subv  = Load(d, temp.ptr<float>(row - box - 1) + col);
          auto diff  = addv - subv;
          auto resv  = prevv + diff / Set(d, cur_len);
          Store(resv, d, dst.ptr<float>(row) + col);
        }

        // 收缩阶段 row = H-box .. H-1
        for (int row = H - box; row < H; ++row) {
          // lenp1 = cur_len; cur_len = cur_len - 1
          float lenp1 = cur_len;
          cur_len     = cur_len - 1.0f;
          auto prevv  = Load(d, dst.ptr<float>(row - 1) + col);
          auto subv   = Load(d, temp.ptr<float>(row - box - 1) + col);
          auto resv   = (prevv * Set(d, lenp1) - subv) / Set(d, cur_len);
          Store(resv, d, dst.ptr<float>(row) + col);
        }
      }  // end vectorized columns

      // 处理剩余列（尾部不足一个向量宽度的列），逐列标量执行
#ifdef _OPENMP
#pragma omp for nowait
#endif
      for (int col = (int)((W / (int)L) * (int)L); col < W; ++col) {
        int len                = box + 1;
        dst.ptr<float>(0)[col] = temp.ptr<float>(0)[col] / (float)len;
        for (int i = 1; i <= box; ++i)
          dst.ptr<float>(0)[col] += temp.ptr<float>(i)[col] / (float)len;

        for (int row = 1; row <= box; ++row) {
          dst.ptr<float>(row)[col] =
              (dst.ptr<float>(row - 1)[col] * (float)len + temp.ptr<float>(row + box)[col]) /
              (float)(len + 1);
          len++;
        }
        for (int row = box + 1; row < H - box; ++row) {
          dst.ptr<float>(row)[col] =
              dst.ptr<float>(row - 1)[col] +
              (temp.ptr<float>(row + box)[col] - temp.ptr<float>(row - box - 1)[col]) / (float)len;
        }
        for (int row = H - box; row < H; ++row) {
          dst.ptr<float>(row)[col] =
              (dst.ptr<float>(row - 1)[col] * (float)len - temp.ptr<float>(row - box - 1)[col]) /
              (float)(len - 1);
          len--;
        }
      }
    }  // end parallel
  }  // end vertical block
}

void boxblur_resamp(const cv::Mat1f& src, cv::Mat1f& dst, cv::Mat1f& temp, int H, int W, int box,
                    int samp) {
  const ScalableTag<float> d;
  const size_t             L    = Lanes(d);
  const auto               zero = Set(d, 0.0f);

// ---------------- Horizontal ----------------
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int row = 0; row < H; row++) {
    const float* src_row  = src.ptr<float>(row);
    float*       temp_row = temp.ptr<float>(row);

    int          len      = box + 1;
    float        tempval  = 0.0f;
    for (int j = 0; j <= box; j++) {
      tempval += src_row[j];
    }
    temp_row[0] = tempval / len;

    for (int col = 1; col <= box; col++) {
      tempval = tempval * len + src_row[col + box];
      len++;
      tempval /= len;
      if (col % samp == 0) {
        temp_row[col / samp] = tempval;
      }
    }

    float oneByLen = 1.0f / static_cast<float>(len);
    for (int col = box + 1; col < W - box; col++) {
      tempval += (src_row[col + box] - src_row[col - box - 1]) * oneByLen;
      if (col % samp == 0) {
        temp_row[col / samp] = tempval;
      }
    }

    for (int col = W - box; col < W; col++) {
      tempval = (tempval * len - src_row[col - box - 1]);
      len--;
      tempval /= len;
      if (col % samp == 0) {
        temp_row[col / samp] = tempval;
      }
    }
  }

  const int Wd = W / samp;

// ---------------- Vertical ----------------
#ifdef _OPENMP
#pragma omp parallel
#endif
  {
#ifdef _OPENMP
#pragma omp for nowait
#endif
    for (int col = 0; col <= Wd - (int)L; col += (int)L) {
      int  len   = box + 1;
      auto lenv  = Set(d, static_cast<float>(len));

      auto acc_v = zero;
      for (int r = 0; r <= box; r++) {
        acc_v = acc_v + Load(d, temp.ptr<float>(r) + col);
      }
      auto out_v = acc_v / lenv;
      Store(out_v, d, dst.ptr<float>(0) + col);

      for (int row = 1; row <= box; row++) {
        auto addv = Load(d, temp.ptr<float>(row + box) + col);
        out_v     = (out_v * lenv + addv);
        len++;
        lenv  = Set(d, static_cast<float>(len));
        out_v = out_v / lenv;
        if (row % samp == 0) {
          Store(out_v, d, dst.ptr<float>(row / samp) + col);
        }
      }

      const auto one_by_lenv = Set(d, 1.0f / static_cast<float>(len));
      for (int row = box + 1; row < H - box; row++) {
        auto addv = Load(d, temp.ptr<float>(row + box) + col);
        auto subv = Load(d, temp.ptr<float>(row - box - 1) + col);
        out_v     = out_v + (addv - subv) * one_by_lenv;
        if (row % samp == 0) {
          Store(out_v, d, dst.ptr<float>(row / samp) + col);
        }
      }

      for (int row = H - box; row < H; row++) {
        auto subv = Load(d, temp.ptr<float>(row - box - 1) + col);
        out_v     = (out_v * lenv - subv);
        len--;
        lenv  = Set(d, static_cast<float>(len));
        out_v = out_v / lenv;
        if (row % samp == 0) {
          Store(out_v, d, dst.ptr<float>(row / samp) + col);
        }
      }
    }

#ifdef _OPENMP
#pragma omp single
#endif
    {
      for (int col = (Wd / (int)L) * (int)L; col < Wd; col++) {
        int   len = box + 1;
        float acc = 0.0f;
        for (int r = 0; r <= box; r++) {
          acc += temp.at<float>(r, col);
        }
        acc /= len;
        dst.at<float>(0, col) = acc;

        for (int row = 1; row <= box; row++) {
          acc = (acc * len + temp.at<float>(row + box, col));
          len++;
          acc /= len;
          if (row % samp == 0) {
            dst.at<float>(row / samp, col) = acc;
          }
        }

        float oneByLen = 1.0f / static_cast<float>(len);
        for (int row = box + 1; row < H - box; row++) {
          acc += (temp.at<float>(row + box, col) - temp.at<float>(row - box - 1, col)) * oneByLen;
          if (row % samp == 0) {
            dst.at<float>(row / samp, col) = acc;
          }
        }

        for (int row = H - box; row < H; row++) {
          acc = (acc * len - temp.at<float>(row - box - 1, col));
          len--;
          acc /= len;
          if (row % samp == 0) {
            dst.at<float>(row / samp, col) = acc;
          }
        }
      }
    }
  }
}
};  // namespace CPU
};  // namespace alcedo

#endif