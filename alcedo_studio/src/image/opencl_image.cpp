//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "image/opencl_image.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "opencl/opencl_api_counters.hpp"

namespace alcedo::opencl {
namespace {

auto CheckedContext() -> OpenClContext& {
  auto& context = OpenClContext::Instance();
  context.Initialize();
  return context;
}

void CheckOpenCl(cl_int error, const char* operation) {
  if (error != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenClImage: ") + operation + " failed with error " +
                             std::to_string(error) + ".");
  }
}

}  // namespace

auto OpenClImage::ByteSize() const -> size_t {
  if (width_ <= 0 || height_ <= 0 || row_bytes_ == 0) {
    return 0;
  }
  return row_bytes_ * static_cast<size_t>(height_);
}

OpenClImage::~OpenClImage() { Release(); }

OpenClImage::OpenClImage(OpenClImage&& other) noexcept
    : buffer_(std::exchange(other.buffer_, nullptr)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      type_(std::exchange(other.type_, -1)),
      row_bytes_(std::exchange(other.row_bytes_, 0)) {}

auto OpenClImage::operator=(OpenClImage&& other) noexcept -> OpenClImage& {
  if (this != &other) {
    Release();
    buffer_    = std::exchange(other.buffer_, nullptr);
    width_     = std::exchange(other.width_, 0);
    height_    = std::exchange(other.height_, 0);
    type_      = std::exchange(other.type_, -1);
    row_bytes_ = std::exchange(other.row_bytes_, 0);
  }
  return *this;
}

auto OpenClImage::Buffer() const -> cl_mem { return buffer_; }

auto OpenClImage::Width() const -> int { return width_; }

auto OpenClImage::Height() const -> int { return height_; }

auto OpenClImage::Type() const -> int { return type_; }

auto OpenClImage::RowBytes() const -> size_t { return row_bytes_; }

auto OpenClImage::Empty() const -> bool {
  return buffer_ == nullptr || width_ <= 0 || height_ <= 0;
}

void OpenClImage::Create(int width, int height, int type) {
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("OpenClImage: image dimensions must be positive.");
  }

  const auto row_bytes = static_cast<size_t>(width) * CV_ELEM_SIZE(type);
  const auto byte_size = row_bytes * static_cast<size_t>(height);
  if (buffer_ != nullptr && width_ == width && height_ == height && type_ == type &&
      row_bytes_ == row_bytes) {
    return;
  }

  Release();

  auto&  context = CheckedContext();
  cl_int error   = CL_SUCCESS;
  buffer_        = clCreateBuffer(context.Context(), CL_MEM_READ_WRITE, byte_size, nullptr, &error);
  CheckOpenCl(error, "clCreateBuffer");
  if (buffer_ == nullptr) {
    throw std::runtime_error("OpenClImage: clCreateBuffer returned a null buffer.");
  }
  NoteOpenClCreateBuffer();

  width_     = width;
  height_    = height;
  type_      = type;
  row_bytes_ = row_bytes;
}

void OpenClImage::Upload(const cv::Mat& cpu_data) {
  if (cpu_data.empty()) {
    throw std::runtime_error("OpenClImage: cannot upload empty CPU image.");
  }

  Create(cpu_data.cols, cpu_data.rows, cpu_data.type());
  auto& context = CheckedContext();

  if (cpu_data.isContinuous() && cpu_data.step == row_bytes_) {
    CheckOpenCl(clEnqueueWriteBuffer(context.Queue(), buffer_, CL_TRUE, 0, ByteSize(),
                                     cpu_data.data, 0, nullptr, nullptr),
                "clEnqueueWriteBuffer");
    NoteOpenClH2DBytes(static_cast<std::uint64_t>(ByteSize()));
    return;
  }

  for (int y = 0; y < height_; ++y) {
    CheckOpenCl(
        clEnqueueWriteBuffer(context.Queue(), buffer_, CL_TRUE, static_cast<size_t>(y) * row_bytes_,
                             row_bytes_, cpu_data.ptr(y), 0, nullptr, nullptr),
        "clEnqueueWriteBuffer(row)");
    NoteOpenClH2DBytes(static_cast<std::uint64_t>(row_bytes_));
  }
}

void OpenClImage::Download(cv::Mat& cpu_data) const {
  if (Empty()) {
    throw std::runtime_error("OpenClImage: cannot download empty image.");
  }

  cpu_data.create(height_, width_, type_);
  auto& context = CheckedContext();

  if (cpu_data.isContinuous() && cpu_data.step == row_bytes_) {
    CheckOpenCl(clEnqueueReadBuffer(context.Queue(), buffer_, CL_TRUE, 0, ByteSize(), cpu_data.data,
                                    0, nullptr, nullptr),
                "clEnqueueReadBuffer");
    NoteOpenClD2HBytes(static_cast<std::uint64_t>(ByteSize()));
    return;
  }

  for (int y = 0; y < height_; ++y) {
    CheckOpenCl(
        clEnqueueReadBuffer(context.Queue(), buffer_, CL_TRUE, static_cast<size_t>(y) * row_bytes_,
                            row_bytes_, cpu_data.ptr(y), 0, nullptr, nullptr),
        "clEnqueueReadBuffer(row)");
    NoteOpenClD2HBytes(static_cast<std::uint64_t>(row_bytes_));
  }
}

void OpenClImage::ShareFrom(const OpenClImage& src) {
  if (this == &src) {
    return;
  }
  if (src.Empty()) {
    Release();
    return;
  }

  CheckOpenCl(clRetainMemObject(src.buffer_), "clRetainMemObject");
  Release();
  buffer_    = src.buffer_;
  width_     = src.width_;
  height_    = src.height_;
  type_      = src.type_;
  row_bytes_ = src.row_bytes_;
}

void OpenClImage::CopyTo(OpenClImage& dst) const {
  if (Empty()) {
    throw std::runtime_error("OpenClImage: cannot copy empty image.");
  }

  dst.Create(width_, height_, type_);
  auto& context = CheckedContext();
  CheckOpenCl(clEnqueueCopyBuffer(context.Queue(), buffer_, dst.buffer_, 0, 0, ByteSize(), 0,
                                  nullptr, nullptr),
              "clEnqueueCopyBuffer");
  CheckOpenCl(clFinish(context.Queue()), "clFinish");
  NoteOpenClQueueFinish();
}

void OpenClImage::ConvertTo(OpenClImage& dst, int type, double alpha, double beta) const {
  if (Empty()) {
    throw std::runtime_error("OpenClImage: cannot convert empty image.");
  }

  cv::Mat cpu_data;
  cv::Mat converted;
  Download(cpu_data);
  cpu_data.convertTo(converted, type, alpha, beta);
  dst.Upload(converted);
}

void OpenClImage::CropTo(OpenClImage& dst, const cv::Rect& crop_rect) const {
  if (Empty()) {
    throw std::runtime_error("OpenClImage: cannot crop an empty image.");
  }
  if (crop_rect.x < 0 || crop_rect.y < 0 || crop_rect.width <= 0 || crop_rect.height <= 0 ||
      crop_rect.x + crop_rect.width > width_ || crop_rect.y + crop_rect.height > height_) {
    throw std::runtime_error("OpenClImage: invalid crop rectangle.");
  }

  dst.Create(crop_rect.width, crop_rect.height, type_);

  auto& context = CheckedContext();
  const size_t src_origin[3] = {
      static_cast<size_t>(crop_rect.x) * CV_ELEM_SIZE(type_),
      static_cast<size_t>(crop_rect.y),
      0};
  const size_t dst_origin[3] = {0, 0, 0};
  const size_t region[3] = {
      static_cast<size_t>(crop_rect.width) * CV_ELEM_SIZE(type_),
      static_cast<size_t>(crop_rect.height),
      1};

  CheckOpenCl(
      clEnqueueCopyBufferRect(context.Queue(), buffer_, dst.buffer_, src_origin, dst_origin, region,
                              row_bytes_, 0, dst.row_bytes_, 0, 0, nullptr, nullptr),
      "clEnqueueCopyBufferRect");
  CheckOpenCl(clFinish(context.Queue()), "clFinish");
  NoteOpenClQueueFinish();
}

void OpenClImage::Release() {
  if (buffer_ != nullptr) {
    clReleaseMemObject(buffer_);
    NoteOpenClReleaseMemObject();
    buffer_ = nullptr;
  }
  width_     = 0;
  height_    = 0;
  type_      = -1;
  row_bytes_ = 0;
}

}  // namespace alcedo::opencl

#endif
