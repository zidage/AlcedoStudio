//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>

#include <opencv2/core/cuda.hpp>

#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {
namespace CUDA {
struct RcdWorkspace {
  cv::cuda::GpuMat r;
  cv::cuda::GpuMat g;
  cv::cuda::GpuMat b;
  cv::cuda::GpuMat vh_dir;
  cv::cuda::GpuMat pq_dir;

  /**
   * @brief Wrap five preallocated F32C1 planes. @ref Reserve then only re-wraps; it does
   *        not call GpuMat::create. Pointers must stay valid for the current submission.
   */
  void BindExternal(void* red, void* green, void* blue, void* vh, void* pq, const cv::Size& size) {
    bound_external_ = true;
    bound_size_     = size;
    r_ptr_          = red;
    g_ptr_          = green;
    b_ptr_          = blue;
    vh_ptr_         = vh;
    pq_ptr_         = pq;
    WrapBoundPlanes();
  }

  void Reserve(const cv::Size& size) {
    if (size.width <= 0 || size.height <= 0) {
      return;
    }
    if (bound_external_) {
      if (size != bound_size_) {
        throw std::runtime_error("RcdWorkspace::Reserve: bound size does not match");
      }
      WrapBoundPlanes();
      return;
    }
    r.create(size, CV_32FC1);
    g.create(size, CV_32FC1);
    b.create(size, CV_32FC1);
    vh_dir.create(size, CV_32FC1);
    pq_dir.create(size, CV_32FC1);
  }

 private:
  void WrapBoundPlanes() {
    const std::size_t step = static_cast<std::size_t>(bound_size_.width) * sizeof(float);
    r                      = cv::cuda::GpuMat(bound_size_.height, bound_size_.width, CV_32FC1, r_ptr_, step);
    g                      = cv::cuda::GpuMat(bound_size_.height, bound_size_.width, CV_32FC1, g_ptr_, step);
    b                      = cv::cuda::GpuMat(bound_size_.height, bound_size_.width, CV_32FC1, b_ptr_, step);
    vh_dir                 = cv::cuda::GpuMat(bound_size_.height, bound_size_.width, CV_32FC1, vh_ptr_, step);
    pq_dir                 = cv::cuda::GpuMat(bound_size_.height, bound_size_.width, CV_32FC1, pq_ptr_, step);
  }

  bool      bound_external_ = false;
  cv::Size  bound_size_{};
  void*     r_ptr_  = nullptr;
  void*     g_ptr_  = nullptr;
  void*     b_ptr_  = nullptr;
  void*     vh_ptr_ = nullptr;
  void*     pq_ptr_ = nullptr;
};

void Bayer2x2ToRGB_RCD(cv::cuda::GpuMat& image, const BayerPattern2x2& pattern,
                       RcdWorkspace* workspace = nullptr, cv::cuda::Stream* stream = nullptr);

void Bayer2x2ToPlanarRGB_RCD(const cv::cuda::GpuMat& raw, const BayerPattern2x2& pattern,
                             RcdWorkspace* workspace = nullptr,
                             cv::cuda::Stream* stream = nullptr);
};
};  // namespace alcedo
