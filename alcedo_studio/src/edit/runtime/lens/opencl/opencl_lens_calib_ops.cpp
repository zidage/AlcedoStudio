//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/lens/opencl/opencl_lens_calib_ops.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "opencl/opencl_context.hpp"
#include "opencl/opencl_geometry_programs.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo::OpenCL::Geometry {
namespace {

struct CropRectPx {
  float left   = 0.0f;
  float right  = 0.0f;
  float top    = 0.0f;
  float bottom = 0.0f;
};

void Check(cl_int error, const char* operation) {
  if (error != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL lens calibration: ") + operation +
                             " failed with error " + std::to_string(error) + ".");
  }
}

class KernelHandle {
 public:
  explicit KernelHandle(const char* kernel_name) {
    cl_int error = CL_SUCCESS;
    kernel_ = clCreateKernel(OpenClProgramLibrary::Instance().GetProgram(kLensCalibProgramName),
                             kernel_name, &error);
    Check(error, "clCreateKernel");
    if (kernel_ == nullptr) {
      throw std::runtime_error("OpenCL lens calibration: clCreateKernel returned a null kernel.");
    }
  }

  ~KernelHandle() {
    if (kernel_ != nullptr) {
      clReleaseKernel(kernel_);
    }
  }

  KernelHandle(const KernelHandle&)                    = delete;
  auto operator=(const KernelHandle&) -> KernelHandle& = delete;

  auto Get() const -> cl_kernel { return kernel_; }

 private:
  cl_kernel kernel_ = nullptr;
};

template <typename T>
void SetKernelArg(cl_kernel kernel, cl_uint index, const T& value) {
  Check(clSetKernelArg(kernel, index, sizeof(T), &value), "clSetKernelArg");
}

void Enqueue2D(cl_kernel kernel, int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  auto&        context        = OpenClContext::Instance();
  const size_t global_size[2] = {static_cast<size_t>(width), static_cast<size_t>(height)};
  Check(clEnqueueNDRangeKernel(context.Queue(), kernel, 2, nullptr, global_size, nullptr, 0,
                               nullptr, nullptr),
        "clEnqueueNDRangeKernel");
}

template <typename T>
void SwapValues(T& a, T& b) {
  const T tmp = a;
  a           = b;
  b           = tmp;
}

auto ResolveCropRectPx(const LensCalibGpuParams& params) -> CropRectPx {
  CropRectPx  rect{};
  const float width  = static_cast<float>(params.dst_width);
  const float height = static_cast<float>(params.dst_height);
  if (width <= 0.0f || height <= 0.0f) {
    return rect;
  }

  if (params.dst_width >= params.dst_height) {
    rect.left   = params.crop_bounds[0] * width;
    rect.right  = params.crop_bounds[1] * width;
    rect.top    = params.crop_bounds[2] * height;
    rect.bottom = params.crop_bounds[3] * height;
  } else {
    rect.left   = params.crop_bounds[2] * width;
    rect.right  = params.crop_bounds[3] * width;
    rect.top    = params.crop_bounds[0] * height;
    rect.bottom = params.crop_bounds[1] * height;
  }

  if (rect.left > rect.right) {
    SwapValues(rect.left, rect.right);
  }
  if (rect.top > rect.bottom) {
    SwapValues(rect.top, rect.bottom);
  }
  return rect;
}

auto ComputeRectCropRoi(const LensCalibGpuParams& params) -> cv::Rect {
  const int width  = params.dst_width;
  const int height = params.dst_height;
  if (width <= 0 || height <= 0) {
    return cv::Rect();
  }

  const CropRectPx rect = ResolveCropRectPx(params);

  int              x0   = static_cast<int>(std::lround(rect.left));
  int              x1   = static_cast<int>(std::lround(rect.right));
  int              y0   = static_cast<int>(std::lround(rect.top));
  int              y1   = static_cast<int>(std::lround(rect.bottom));

  if (x0 > x1) {
    std::swap(x0, x1);
  }
  if (y0 > y1) {
    std::swap(y0, y1);
  }

  x0 = std::clamp(x0, 0, width - 1);
  y0 = std::clamp(y0, 0, height - 1);
  x1 = std::clamp(x1, x0 + 1, width);
  y1 = std::clamp(y1, y0 + 1, height);
  return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
}

void DispatchVignetting(opencl::OpenClImage& image, const LensCalibGpuParams& params) {
  KernelHandle kernel(kLensVignettingKernelName);
  cl_mem       buffer = image.Buffer();
  const int    stride = static_cast<int>(image.RowBytes() / (sizeof(float) * 4U));
  SetKernelArg(kernel.Get(), 0, buffer);
  SetKernelArg(kernel.Get(), 1, stride);
  SetKernelArg(kernel.Get(), 2, params);
  Enqueue2D(kernel.Get(), image.Width(), image.Height());
}

void DispatchWarp(const opencl::OpenClImage& src, opencl::OpenClImage& dst,
                  const LensCalibGpuParams& params) {
  dst.Create(src.Width(), src.Height(), src.Type());
  KernelHandle kernel(kLensWarpKernelName);
  cl_mem       src_buffer = src.Buffer();
  cl_mem       dst_buffer = dst.Buffer();
  const int    src_stride = static_cast<int>(src.RowBytes() / (sizeof(float) * 4U));
  const int    dst_stride = static_cast<int>(dst.RowBytes() / (sizeof(float) * 4U));
  SetKernelArg(kernel.Get(), 0, src_buffer);
  SetKernelArg(kernel.Get(), 1, src_stride);
  SetKernelArg(kernel.Get(), 2, dst_buffer);
  SetKernelArg(kernel.Get(), 3, dst_stride);
  SetKernelArg(kernel.Get(), 4, params);
  Enqueue2D(kernel.Get(), dst.Width(), dst.Height());
}

}  // namespace

void ApplyLensCalibration(opencl::OpenClImage& image, const LensCalibGpuParams& params) {
  if (image.Empty()) {
    return;
  }
  if (image.Type() != CV_32FC4) {
    throw std::runtime_error("OpenCL lens calibration expects CV_32FC4 input.");
  }

  LensCalibGpuParams launch = params;
  if (launch.src_width <= 0 || launch.src_height <= 0) {
    launch.src_width  = image.Width();
    launch.src_height = image.Height();
  }
  if (launch.dst_width <= 0 || launch.dst_height <= 0) {
    launch.dst_width  = image.Width();
    launch.dst_height = image.Height();
  }

  const bool has_vignetting = (launch.apply_vignetting != 0);
  const bool has_warp       = (launch.apply_distortion != 0 || launch.apply_tca != 0 ||
                         launch.apply_projection != 0 || launch.apply_crop_circle != 0);
  const bool has_rect_crop =
      (launch.apply_crop != 0 &&
       static_cast<LensCalibCropMode>(launch.crop_mode) == LensCalibCropMode::RECTANGLE);
  if (!has_vignetting && !has_warp && !has_rect_crop) {
    return;
  }

  if (has_vignetting) {
    DispatchVignetting(image, launch);
  }

  if (has_warp) {
    opencl::OpenClImage warped;
    DispatchWarp(image, warped, launch);
    image = std::move(warped);
  }

  if (has_rect_crop) {
    const cv::Rect roi = ComputeRectCropRoi(launch);
    if (roi.width > 0 && roi.height > 0 &&
        (roi.width < image.Width() || roi.height < image.Height())) {
      opencl::OpenClImage cropped;
      image.CropTo(cropped, roi);
      image = std::move(cropped);
    }
  }

  Check(clFinish(OpenClContext::Instance().Queue()), "clFinish");
}

}  // namespace alcedo::OpenCL::Geometry

#endif
