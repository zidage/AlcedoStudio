//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <variant>

#include "image/image_buffer.hpp"

namespace alcedo {

struct Pixel {
  float r_, g_, b_, a_;

  Pixel operator*(float scalar) const {
    return Pixel{r_ * scalar, g_ * scalar, b_ * scalar, a_ * scalar};
  }
  Pixel operator+(const Pixel& other) const {
    return Pixel{r_ + other.r_, g_ + other.g_, b_ + other.b_, a_ + other.a_};
  }
  Pixel operator-(const Pixel& other) const {
    return Pixel{r_ - other.r_, g_ - other.g_, b_ - other.b_, a_ - other.a_};
  }
  Pixel& operator+=(const Pixel& other) {
    r_ += other.r_;
    g_ += other.g_;
    b_ += other.b_;
    a_ += other.a_;
    return *this;
  }
};

struct Rect {
  int  x_, y_, width_, height_;  // (x, y) is the top-left corner

  Rect expand(int margin) const {
    Rect new_rect   = *this;
    new_rect.x_      = std::max(0, x_ - margin);
    new_rect.y_      = std::max(0, y_ - margin);
    new_rect.width_  = width_ + 2 * margin;
    new_rect.height_ = height_ + 2 * margin;
    return new_rect;
  }
};

struct Tile {
  Pixel*                            ptr_   = nullptr;
  int                               width_ = 0, height_ = 0;
  int                               channels_ = 4;

  int                               x0_ = 0, y0_ = 0;  // expanded tile origin in image coords
  int                               halo_     = 0;

  // Inner (non-halo) region in image coords + size
  int                               inner_x0_ = 0, inner_y0_ = 0;
  int                               inner_width_ = 0, inner_height_ = 0;

  // Inner (non-halo) region offset within the expanded tile (local coords)
  int                               inner_off_x_ = 0, inner_off_y_ = 0;

  static constexpr std::align_val_t kAlign{64};

  explicit Tile(int x0, int y0, int width, int height, int halo, int inner_x0, int inner_y0,
                int inner_width, int inner_height, int inner_off_x, int inner_off_y)
      : width_(width),
        height_(height),
        x0_(x0),
        y0_(y0),
        halo_(halo),
        inner_x0_(inner_x0),
        inner_y0_(inner_y0),
        inner_width_(inner_width),
        inner_height_(inner_height),
        inner_off_x_(inner_off_x),
        inner_off_y_(inner_off_y) {
    const size_t count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    ptr_               = static_cast<Pixel*>(::operator new[](count * sizeof(Pixel), kAlign));
  }

  ~Tile() {
    if (ptr_) {
      ::operator delete[](ptr_, kAlign);
      ptr_ = nullptr;
    }
  }

  Tile(const Tile&)            = delete;
  Tile& operator=(const Tile&) = delete;

  Tile(Tile&& other) noexcept { *this = std::move(other); }
  Tile& operator=(Tile&& other) noexcept {
    if (this == &other) return *this;
    // free current
    if (ptr_) ::operator delete[](ptr_, kAlign);

    // move fields
    ptr_          = other.ptr_;
    width_        = other.width_;
    height_       = other.height_;
    channels_     = other.channels_;
    x0_           = other.x0_;
    y0_           = other.y0_;
    halo_         = other.halo_;
    inner_x0_     = other.inner_x0_;
    inner_y0_     = other.inner_y0_;
    inner_width_  = other.inner_width_;
    inner_height_ = other.inner_height_;
    inner_off_x_  = other.inner_off_x_;
    inner_off_y_  = other.inner_off_y_;

    // null out source
    other.ptr_    = nullptr;
    other.width_ = other.height_ = 0;
    other.inner_width_ = other.inner_height_ = 0;
    return *this;
  }

  inline static auto CopyFrom(const cv::Mat& img_data, const Rect& region, int halo) -> Tile {
    // Expect float image data (your code uses ptr<float>)
    CV_Assert(img_data.depth() == CV_32F);
    CV_Assert(img_data.channels() == 3 || img_data.channels() == 4);

    const Rect expanded = region.expand(halo);

    const int  roi_x    = expanded.x_;
    const int  roi_y    = expanded.y_;
    const int  roi_w    = std::min(expanded.width_, img_data.cols - roi_x);
    const int  roi_h    = std::min(expanded.height_, img_data.rows - roi_y);

    cv::Rect   roi(roi_x, roi_y, roi_w, roi_h);
    cv::Mat    tile_mat    = img_data(roi);

    // inner offset within expanded ROI (local coords)
    const int  inner_off_x = region.x_ - roi_x;
    const int  inner_off_y = region.y_ - roi_y;

    Tile tile(roi_x, roi_y, tile_mat.cols, tile_mat.rows, halo, region.x_, region.y_, region.width_,
              region.height_, inner_off_x, inner_off_y);

    const int ch = tile_mat.channels();
    for (int i = 0; i < tile_mat.rows; ++i) {
      const float* src_row = tile_mat.ptr<const float>(i);
      for (int j = 0; j < tile_mat.cols; ++j) {
        Pixel& p = tile.ptr_[i * tile_mat.cols + j];
        p.r_      = src_row[j * ch + 0];
        p.g_      = src_row[j * ch + 1];
        p.b_      = src_row[j * ch + 2];
        p.a_      = (ch == 4) ? src_row[j * ch + 3] : 1.0f;
      }
    }
    return tile;  // safe (move-enabled, copy-disabled)
  }

  inline static auto CopyInto(cv::Mat& img_data, const Tile& tile) -> void {
    CV_Assert(img_data.depth() == CV_32F);
    CV_Assert(img_data.channels() == 3 || img_data.channels() == 4);

    // Write ONLY the inner region back to the image (simpler + avoids overlap confusion)
    const int dst_w = std::min(tile.inner_width_, img_data.cols - tile.inner_x0_);
    const int dst_h = std::min(tile.inner_height_, img_data.rows - tile.inner_y0_);
    if (dst_w <= 0 || dst_h <= 0) return;

    cv::Rect  inner_roi(tile.inner_x0_, tile.inner_y0_, dst_w, dst_h);
    cv::Mat   dst = img_data(inner_roi);

    const int ch  = dst.channels();
    for (int i = 0; i < dst_h; ++i) {
      float*    dst_row = dst.ptr<float>(i);
      const int src_y   = tile.inner_off_y_ + i;
      for (int j = 0; j < dst_w; ++j) {
        const int    src_x  = tile.inner_off_x_ + j;
        const Pixel& p      = tile.at(src_y, src_x);
        dst_row[j * ch + 0] = p.r_;
        dst_row[j * ch + 1] = p.g_;
        dst_row[j * ch + 2] = p.b_;
        if (ch == 4) dst_row[j * ch + 3] = p.a_;
      }
    }
  }

  inline Pixel&       at(int y, int x) { return ptr_[y * width_ + x]; }
  inline const Pixel& at(int y, int x) const { return ptr_[y * width_ + x]; }

  inline Pixel&       at_inner(int y, int x) {
    return ptr_[(y + inner_off_y_) * width_ + (x + inner_off_x_)];
  }
  inline const Pixel& at_inner(int y, int x) const {
    return ptr_[(y + inner_off_y_) * width_ + (x + inner_off_x_)];
  }
};

struct ImageAccessor {
  Tile* tile_;

  ImageAccessor(Tile* tile) : tile_(tile) {}
  /**
   * @brief Access pixel at (x, y) with border replication
   *
   * @param x
   * @param y
   * @return Pixel
   */
  Pixel at(int x, int y) const {
    // Use border replication for out-of-bounds access
    return tile_->ptr_[std::clamp(y, 0, tile_->height_ - 1) * tile_->width_ +
                       std::clamp(x, 0, tile_->width_ - 1)];
  }
};

struct PointOpTag {};
struct NeighborOpTag {};

struct GPUPointOpTag {};
struct GPUNeighborOpTag {};

};  // namespace alcedo
