//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <opencv2/core.hpp>

#include "opencl/opencl_context.hpp"

namespace alcedo::opencl {

class OpenClImage {
 private:
  cl_mem buffer_    = nullptr;
  int    width_     = 0;
  int    height_    = 0;
  int    type_      = -1;
  size_t row_bytes_ = 0;

  auto   ByteSize() const -> size_t;

 public:
  OpenClImage() = default;
  ~OpenClImage();

  OpenClImage(const OpenClImage&)                    = delete;
  auto operator=(const OpenClImage&) -> OpenClImage& = delete;

  OpenClImage(OpenClImage&& other) noexcept;
  auto operator=(OpenClImage&& other) noexcept -> OpenClImage&;

  /**
   * @brief Retain and wrap an existing tightly packed OpenCL image buffer.
   *
   * The returned image owns one retained reference. It does not copy pixels.
   */
  static auto Wrap(cl_mem buffer, int width, int height, int type) -> OpenClImage;

  auto Buffer() const -> cl_mem;
  auto Width() const -> int;
  auto Height() const -> int;
  auto Type() const -> int;
  auto RowBytes() const -> size_t;
  auto Empty() const -> bool;

  void Create(int width, int height, int type);
  void Upload(const cv::Mat& cpu_data);
  void Download(cv::Mat& cpu_data) const;
  void ShareFrom(const OpenClImage& src);
  void CopyTo(OpenClImage& dst) const;
  void ConvertTo(OpenClImage& dst, int type, double alpha = 1.0, double beta = 0.0) const;
  void CropTo(OpenClImage& dst, const cv::Rect& crop_rect) const;
  void Release();
};

}  // namespace alcedo::opencl

#endif
